#ifndef WSP_CHASE_LEV_DEQUE_HPP
#define WSP_CHASE_LEV_DEQUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "wsp/task.hpp"

namespace wsp {

// Phase 2 capstone: a lock-free, single-producer / multi-consumer work-stealing
// deque — the classic Chase–Lev algorithm (Chase & Lev 2005; memory-ordering
// per Lê, Pop, Cohen & Nardelli, "Correct and Efficient Work-Stealing for Weak
// Memory Models", PPoPP'13).
//
//   bottom (owner end)                              top (thief end)
//        v                                               v
//     [ . . . x x x x x x x x . . . ]  (a fixed-capacity ring buffer)
//        push_bottom / pop_bottom  (LIFO, owner, no lock)
//                                       steal_top  (FIFO, thieves, CAS)
//
// Concurrency contract (mirrors IntrusiveDeque so it can stand in for it):
//   * Exactly ONE owner thread calls push_bottom / pop_bottom.
//   * ANY number of thief threads call steal_top.
//   * The owner never blocks; the only synchronization is a CAS on `top` that
//     resolves the single case where a pop and a steal contend for the *last*
//     element. There is no lock anywhere on any path.
//
// Why fixed capacity? A growable Chase–Lev must keep old buffers alive until no
// thief can still be reading them — a memory-reclamation problem (hazard
// pointers / epochs) that is subtle and easy to get wrong. A fixed ring sidesteps
// it entirely: push_bottom returns false when full, exactly like
// IntrusiveDeque::try_push_back, so the pool's existing overflow path is the
// natural fallback. A slot is therefore never overwritten while a thief might
// still read it (that would need `capacity` more pushes past a still-live index,
// which the full-check forbids), so stored TaskNode* are always valid — no ABA
// on the buffer.
//
// This deque is a self-contained demonstration of the lock-free technique. The
// pool keeps the lock-based IntrusiveDeque (with batch stealing) as its default,
// locally-verifiable path; see DESIGN.md §4.6. Every atomic below carries the
// reason for its ordering. NOTE: it is validated here by an exact-count
// owner-vs-thieves stress test (which catches any lost/duplicated node), and is
// gated under ThreadSanitizer on Linux before being relied upon — TSan does not
// run on the Windows dev box.
//
// Ownership: stores non-owning TaskNode*. Whoever pops or steals a node owns it
// and must run/free it. The buffer itself owns nothing.
class ChaseLevDeque {
public:
    // capacity is rounded up to a power of two (>= 2) for cheap index masking.
    explicit ChaseLevDeque(std::size_t capacity = 1024)
        : cap_(round_up_pow2(capacity)),
          mask_(cap_ - 1),
          buf_(std::make_unique<std::atomic<TaskNode*>[]>(cap_)) {}

    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    // OWNER: push a node onto the bottom. Returns false (taking no ownership) if
    // the deque is full, so the caller can route to an overflow queue.
    bool push_bottom(TaskNode* node) {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed);
        // acquire: we must observe thieves' completed steals (their top stores)
        // so our size check is not stale and we never overwrite a live slot.
        const std::int64_t t = top_.load(std::memory_order_acquire);
        if (b - t >= static_cast<std::int64_t>(cap_)) return false;  // full
        buf_[b & mask_].store(node, std::memory_order_relaxed);
        // release: publishes the slot write above. A thief that later
        // acquire-loads this new `bottom` is guaranteed to see the node.
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    // OWNER: pop the most-recently-pushed node from the bottom (LIFO). Returns
    // nullptr if empty.
    TaskNode* pop_bottom() {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        // seq_cst fence: globally orders our `bottom` decrement *before* we read
        // `top`. It pairs with the seq_cst fence in steal_top so that the owner
        // and a thief cannot both believe they took the last element.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_relaxed);

        if (t > b) {                 // was empty
            bottom_.store(b + 1, std::memory_order_relaxed);  // restore
            return nullptr;
        }
        TaskNode* node = buf_[b & mask_].load(std::memory_order_relaxed);
        if (t != b) return node;     // >1 element: no thief can race us, done

        // Exactly one element (t == b): a thief may be stealing it right now.
        // Settle the tie with a CAS on `top`; whoever advances top wins.
        bool won = top_.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
        if (!won) node = nullptr;    // a thief got it
        bottom_.store(b + 1, std::memory_order_relaxed);  // deque is now empty
        return node;
    }

    // THIEF: steal the oldest node from the top (FIFO). Returns nullptr if empty
    // or if it lost the race for the element to the owner / another thief.
    TaskNode* steal_top() {
        // acquire: synchronizes with other thieves' top CAS so we start from a
        // top no older than the last successful steal.
        std::int64_t t = top_.load(std::memory_order_acquire);
        // seq_cst fence: pairs with pop_bottom's fence (see there) and orders
        // this top read before the bottom read below.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // acquire: synchronizes with push_bottom's release store of `bottom`,
        // so the slot read below sees a fully-published node.
        const std::int64_t b = bottom_.load(std::memory_order_acquire);
        if (t >= b) return nullptr;  // empty

        TaskNode* node = buf_[t & mask_].load(std::memory_order_relaxed);
        // Claim element t by advancing top. seq_cst to take part in the single
        // total order with pop_bottom. On failure another taker won it; we read
        // `node` before the CAS, but only return it if we actually win.
        if (!top_.compare_exchange_strong(
                t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return nullptr;
        }
        return node;
    }

    // Snapshot size (owner-biased). For diagnostics/tests only; racy by nature.
    std::size_t size() const {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed);
        const std::int64_t t = top_.load(std::memory_order_relaxed);
        return b > t ? static_cast<std::size_t>(b - t) : 0;
    }
    bool empty() const { return size() == 0; }
    std::size_t capacity() const { return cap_; }

private:
    static std::size_t round_up_pow2(std::size_t x) {
        if (x < 2) return 2;
        --x;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        x |= x >> 32;
        return x + 1;
    }

    const std::size_t cap_;
    const std::size_t mask_;
    std::unique_ptr<std::atomic<TaskNode*>[]> buf_;
    // top advances on steal (and the last-element pop); bottom on push/pop. Both
    // are monotonic over the deque's life (bottom dips transiently inside
    // pop_bottom), so 64 bits never wrap and there is no ABA on the indices.
    std::atomic<std::int64_t> top_{0};
    std::atomic<std::int64_t> bottom_{0};
};

}  // namespace wsp

#endif  // WSP_CHASE_LEV_DEQUE_HPP
