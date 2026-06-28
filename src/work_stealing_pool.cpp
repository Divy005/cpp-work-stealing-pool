#include "wsp/work_stealing_pool.hpp"

#include <cassert>
#include <cstdint>
#include <utility>

namespace wsp {
namespace {

// Per-thread context for worker threads. Lets enqueue() detect that it is being
// called from inside a task (recursive submission) and route the new task to the
// caller's own deque. The `pool` pointer disambiguates in the (unusual) case of
// nested pools sharing a thread.
struct WorkerContext {
    const void* pool = nullptr;
    std::size_t index = 0;
    std::uint64_t rng = 0;  // xorshift state, seeded per worker
};
thread_local WorkerContext t_ctx;

// Fast, good-enough PRNG for victim selection. xorshift64.
inline std::uint64_t next_rand(std::uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

// CPU relax hint: lets a spinning thief avoid pegging the core and, on x86,
// yields the hyperthread's pipeline so the busy producer/owner makes progress.
inline void cpu_relax() {
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Idle backoff ladder for a worker that still sees pending_ > 0 (work exists,
// just not grabbable this instant). A bounded spin with an exponentially growing
// pause budget, escalating to cooperative yields. We deliberately never sleep
// while pending_ > 0 so a deep recursive computation keeps every worker
// saturated; sleeping is reserved for the genuinely-idle case below.
constexpr int kMaxSpinShift = 10;  // pause budget caps at (1 << 10) == 1024
constexpr int kYieldAfter = 20;    // after this many empty rounds, also yield()

}  // namespace

WorkStealingPool::WorkStealingPool(std::size_t workers,
                                   std::size_t deque_capacity)
    : deque_capacity_(deque_capacity) {
    if (workers == 0) {
        workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 1;
    }
    deques_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        deques_.push_back(std::make_unique<IntrusiveDeque>(deque_capacity_));
    }
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
    }
}

WorkStealingPool::~WorkStealingPool() { shutdown(ShutdownMode::Drain); }

void WorkStealingPool::shutdown(ShutdownMode mode) {
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        if (joined_) return;  // idempotent: already shut down
        accepting_.store(false, std::memory_order_release);
        if (mode == ShutdownMode::Cancel)
            cancel_.store(true, std::memory_order_release);
        stop_ = true;
    }
    idle_cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }

    // Workers have joined, so the deques and overflow list are ours alone now.
    // Free any nodes that were never executed: none on Drain (workers ran them
    // all before exiting), the discarded backlog on Cancel. Decrement pending_
    // per freed node so a stray wait() observes zero instead of hanging.
    for (auto& d : deques_) {
        while (TaskNode* n = d->pop_back()) {
            delete n;
            pending_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    for (TaskNode* n = overflow_head_; n != nullptr;) {
        TaskNode* next = n->next;
        delete n;
        n = next;
        pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
    overflow_head_ = overflow_tail_ = nullptr;

    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
        joined_ = true;
    }
    {
        std::lock_guard<std::mutex> lk(done_mtx_);
    }
    done_cv_.notify_all();
}

void WorkStealingPool::push_overflow(TaskNode* node) {
    node->next = nullptr;
    std::lock_guard<std::mutex> lk(overflow_mtx_);
    if (overflow_tail_ == nullptr) {
        overflow_head_ = overflow_tail_ = node;
    } else {
        overflow_tail_->next = node;
        overflow_tail_ = node;
    }
}

TaskNode* WorkStealingPool::pop_overflow() {
    std::lock_guard<std::mutex> lk(overflow_mtx_);
    TaskNode* node = overflow_head_;
    if (node == nullptr) return nullptr;
    overflow_head_ = node->next;
    if (overflow_head_ == nullptr) overflow_tail_ = nullptr;
    node->next = node->prev = nullptr;
    return node;
}

void WorkStealingPool::wake_one_worker() {
    // Only do the (relatively expensive) wakeup dance if a worker is actually
    // asleep. The seq_cst load here pairs with the seq_cst store of
    // idle_workers_ in worker_loop: by the Dekker handshake, if we observe zero
    // sleepers then any worker about to sleep is guaranteed to observe our
    // already-incremented pending_ and not sleep. So skipping is safe.
    if (idle_workers_.load(std::memory_order_seq_cst) == 0) return;

    // A worker is (about to be) asleep. Acquire the idle mutex briefly: this
    // serializes against a worker mid-decision — either it has not yet entered
    // wait() (it will re-check pending_ and stay awake) or it is already waiting
    // with the lock released (so our notify reaches it). No lost wakeup.
    {
        std::lock_guard<std::mutex> lk(idle_mtx_);
    }
    idle_cv_.notify_one();
}

void WorkStealingPool::enqueue(Task task) {
    if (!accepting_.load(std::memory_order_acquire)) return;  // shut down
    auto* node = new TaskNode(std::move(task));
    // Account for the task before it becomes visible to any worker, so wait()
    // cannot observe a premature zero. seq_cst (not relaxed) because this store
    // is the producer half of the Dekker handshake with a worker deciding to
    // sleep: it must be globally ordered against the idle_workers_ load in
    // wake_one_worker(). On x86 fetch_add is already a locked RMW, so seq_cst is
    // free here.
    pending_.fetch_add(1, std::memory_order_seq_cst);

    bool placed = false;
    if (t_ctx.pool == this) {
        // Recursive submission from a worker: keep it local and cache-hot.
        placed = deques_[t_ctx.index]->try_push_back(node);
    } else {
        // External submission: round-robin across deques to spread lock load.
        const std::size_t n = deques_.size();
        const std::size_t start = rr_.fetch_add(1, std::memory_order_relaxed) % n;
        placed = deques_[start]->try_push_back(node);
    }

    if (!placed) {
        // Deque full -> overflow fallback (never drop a task).
        overflow_pushes_.fetch_add(1, std::memory_order_relaxed);
        push_overflow(node);
    }

    wake_one_worker();
}

TaskNode* WorkStealingPool::try_steal(std::size_t self) {
    const std::size_t n = deques_.size();
    if (n <= 1) return nullptr;
    steal_attempts_.fetch_add(1, std::memory_order_relaxed);
    // Randomized starting victim avoids convoys where every thief hammers the
    // same deque.
    const std::size_t start =
        static_cast<std::size_t>(next_rand(t_ctx.rng)) % n;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t victim = (start + k) % n;
        if (victim == self) continue;
        if (TaskNode* node = deques_[victim]->steal_front()) {
            steals_.fetch_add(1, std::memory_order_relaxed);
            return node;
        }
    }
    return nullptr;
}

void WorkStealingPool::on_task_done() {
    // Decrement after the task fully completes. acq_rel so the completed work
    // happens-before wait() observes zero. If we drove pending_ to zero, wake
    // wait(); take done_mtx_ first so the wakeup cannot be lost.
    if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lk(done_mtx_);
        done_cv_.notify_all();
    }
}

void WorkStealingPool::worker_loop(std::size_t index) {
    t_ctx.pool = this;
    t_ctx.index = index;
    t_ctx.rng = 0x9E3779B97F4A7C15ull ^ (index + 1);  // nonzero per-worker seed

    int idle_spins = 0;  // consecutive empty rounds, drives the backoff
    for (;;) {
        // Cancel shutdown: stop pulling new work at once. Any task already
        // running finished above, so this honours "in-flight tasks complete"
        // while the queued backlog is left for shutdown() to discard.
        if (cancel_.load(std::memory_order_acquire)) break;

        // 1. Own work first (LIFO, hottest in cache).
        TaskNode* node = deques_[index]->pop_back();
        // 2. Steal from a random victim.
        if (node == nullptr) node = try_steal(index);
        // 3. Global overflow fallback.
        if (node == nullptr) node = pop_overflow();

        if (node != nullptr) {
            node->task();
            delete node;
            on_task_done();
            idle_spins = 0;
            continue;
        }

        // 4. Nothing found this pass. While work clearly exists somewhere
        // (pending_ > 0) we stay awake and keep trying — it is cheap to grab
        // once it surfaces, and this is exactly the behaviour that lets a deep
        // recursive/divide-and-conquer computation keep all workers busy. Every
        // failed steal locks victim deques, though, so we back off with an
        // exponentially growing pause (escalating to yield()) to keep a thief
        // storm from starving the threads doing real work.
        if (pending_.load(std::memory_order_acquire) > 0) {
            // Bounded exponential spin, escalating to yields (see the ladder
            // constants above). Never sleep here: work exists and may surface
            // at any moment, and grabbing it cheaply is what keeps recursive
            // computations saturating all workers.
            ++idle_spins;
            const int shift =
                idle_spins < kMaxSpinShift ? idle_spins : kMaxSpinShift;
            for (int i = 0; i < (1 << shift); ++i) cpu_relax();
            if (idle_spins > kYieldAfter) std::this_thread::yield();
            continue;
        }
        idle_spins = 0;

        // Genuinely nothing pending: sleep so we don't burn a core. Publish that
        // we are about to sleep, then RE-CHECK pending_ with seq_cst. This is
        // the worker half of the Dekker handshake: the idle_workers_ store is
        // globally ordered before this load, so if a producer's enqueue raced
        // us, either it sees idle_workers_ > 0 (and wakes us) or we observe its
        // pending_ increment here. Either way no wakeup is lost.
        std::unique_lock<std::mutex> lk(idle_mtx_);
        idle_workers_.fetch_add(1, std::memory_order_seq_cst);
        if (pending_.load(std::memory_order_seq_cst) == 0 && !stop_) {
            sleeps_.fetch_add(1, std::memory_order_relaxed);
            idle_cv_.wait(lk, [this] {
                return stop_ || pending_.load(std::memory_order_acquire) > 0;
            });
        }
        idle_workers_.fetch_sub(1, std::memory_order_relaxed);
        if (stop_ && pending_.load(std::memory_order_acquire) == 0) break;
    }
}

void WorkStealingPool::wait() {
    std::unique_lock<std::mutex> lk(done_mtx_);
    done_cv_.wait(lk, [this] {
        return pending_.load(std::memory_order_acquire) == 0;
    });
}

}  // namespace wsp
