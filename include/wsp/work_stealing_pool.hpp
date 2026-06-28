#ifndef WSP_WORK_STEALING_POOL_HPP
#define WSP_WORK_STEALING_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "wsp/intrusive_deque.hpp"
#include "wsp/task.hpp"
#include "wsp/thread_pool.hpp"

namespace wsp {

// Phase 1: a work-stealing thread pool.
//
//   * Each worker owns an IntrusiveDeque (LIFO at the back for the owner).
//   * Idle workers steal from the FRONT of a randomly chosen victim deque.
//   * A single overflow queue catches tasks when a deque is at capacity and is
//     also drained by idle workers, guaranteeing no task is ever dropped.
//
// See DESIGN.md §3 for the full algorithm, invariants and memory-ordering
// rationale.
class WorkStealingPool final : public ThreadPool {
public:
    // workers == 0  -> hardware_concurrency(); deque_capacity == 0 -> unbounded.
    explicit WorkStealingPool(std::size_t workers = 0,
                              std::size_t deque_capacity = 4096);
    ~WorkStealingPool() override;

    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    void enqueue(Task task) override;
    void wait() override;
    void shutdown(ShutdownMode mode = ShutdownMode::Drain) override;
    std::size_t worker_count() const override { return workers_.size(); }
    const char* name() const override { return "work-stealing"; }

    // Observability (used by the eval harness / tests). Monotonic counters,
    // relaxed atomics — they are stats only and never gate correctness.
    // Number of successful steal *operations* (each a batch of >= 1 task).
    std::uint64_t steals() const {
        return steals_.load(std::memory_order_relaxed);
    }
    // Total tasks moved by those steals (batch stealing makes this >= steals()).
    std::uint64_t stolen_tasks() const {
        return stolen_tasks_.load(std::memory_order_relaxed);
    }
    // Total steal sweeps attempted; steals()/steal_attempts() is the hit rate.
    std::uint64_t steal_attempts() const {
        return steal_attempts_.load(std::memory_order_relaxed);
    }
    // Times a worker parked on the idle CV (truly no pending work).
    std::uint64_t sleeps() const {
        return sleeps_.load(std::memory_order_relaxed);
    }
    std::uint64_t overflow_pushes() const {
        return overflow_pushes_.load(std::memory_order_relaxed);
    }
    // Times an external producer backed off because the overflow queue was at
    // its soft cap (a sign the pool is being overwhelmed by submissions).
    std::uint64_t overflow_throttles() const {
        return overflow_throttles_.load(std::memory_order_relaxed);
    }

private:
    void worker_loop(std::size_t index);
    TaskNode* try_steal(std::size_t self);
    void push_overflow(TaskNode* node);
    TaskNode* pop_overflow();
    void throttle_overflow();
    void wake_one_worker();
    void on_task_done();

    std::vector<std::unique_ptr<IntrusiveDeque>> deques_;
    std::vector<std::thread> workers_;
    std::size_t deque_capacity_;
    // Soft cap on the overflow queue. 0 == unbounded (when deques are unbounded
    // overflow is never used). Exceeding it makes external producers back off.
    std::size_t overflow_cap_;

    // Overflow queue: intrusive FIFO guarded by its own mutex.
    std::mutex overflow_mtx_;
    TaskNode* overflow_head_ = nullptr;
    TaskNode* overflow_tail_ = nullptr;
    // Current overflow length. Updated under overflow_mtx_ but read lock-free by
    // throttle_overflow(), so it is atomic to keep that read race-free.
    std::atomic<std::size_t> overflow_size_{0};

    // Round-robin target for external submissions.
    std::atomic<std::size_t> rr_{0};

    // Completion tracking: incremented before a task is published, decremented
    // after it finishes. wait() blocks until this hits zero.
    std::atomic<std::size_t> pending_{0};
    std::mutex done_mtx_;
    std::condition_variable done_cv_;

    // Idle/wakeup: workers with no work sleep here until woken by a producer or
    // by shutdown. idle_workers_ counts how many are currently asleep so the
    // producer can skip the wakeup machinery entirely when nobody is waiting
    // (the common case under load) — see enqueue()/worker_loop() for the
    // seq_cst handshake that keeps this lost-wakeup-free.
    std::mutex idle_mtx_;
    std::condition_variable idle_cv_;
    std::atomic<int> idle_workers_{0};
    bool stop_ = false;        // shutdown requested (guarded by idle_mtx_)
    bool joined_ = false;      // shutdown() completed (idempotency guard)
    // Cancel mode: workers stop pulling new work immediately. Read on the hot
    // path each loop, so it is an atomic rather than guarded by idle_mtx_.
    std::atomic<bool> cancel_{false};
    // Best-effort gate: enqueue() becomes a no-op once shutdown has begun.
    std::atomic<bool> accepting_{true};

    std::atomic<std::uint64_t> steals_{0};
    std::atomic<std::uint64_t> stolen_tasks_{0};
    std::atomic<std::uint64_t> steal_attempts_{0};
    std::atomic<std::uint64_t> sleeps_{0};
    std::atomic<std::uint64_t> overflow_pushes_{0};
    std::atomic<std::uint64_t> overflow_throttles_{0};
};

}  // namespace wsp

#endif  // WSP_WORK_STEALING_POOL_HPP
