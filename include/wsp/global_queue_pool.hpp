#ifndef WSP_GLOBAL_QUEUE_POOL_HPP
#define WSP_GLOBAL_QUEUE_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "wsp/task.hpp"
#include "wsp/thread_pool.hpp"

namespace wsp {

// Phase 0 baseline: a classic thread pool backed by a single shared FIFO queue
// guarded by one mutex. Correct and simple, but every enqueue and every dequeue
// contends on the same lock, so it does not scale past a handful of cores. It
// exists to (a) establish a correctness baseline and (b) give the work-stealing
// pool a number to beat.
//
// The intrusive TaskNode list is reused here as the FIFO storage so the two
// pools share allocation behaviour and the comparison stays apples-to-apples.
class GlobalQueuePool final : public ThreadPool {
public:
    explicit GlobalQueuePool(std::size_t workers = 0);
    ~GlobalQueuePool() override;

    GlobalQueuePool(const GlobalQueuePool&) = delete;
    GlobalQueuePool& operator=(const GlobalQueuePool&) = delete;

    void enqueue(Task task) override;
    void wait() override;
    std::size_t worker_count() const override { return workers_.size(); }
    const char* name() const override { return "global-queue"; }

private:
    void worker_loop();

    // ---- shared FIFO queue (head = oldest, tail = newest) ----
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;  // workers sleep here when idle
    TaskNode* head_ = nullptr;
    TaskNode* tail_ = nullptr;
    bool stop_ = false;

    // ---- completion tracking ----
    // pending_ counts tasks submitted but not yet finished. Atomic so the
    // decrement on the worker side does not need the queue lock.
    std::atomic<std::size_t> pending_{0};
    std::mutex done_mtx_;
    std::condition_variable done_cv_;

    std::vector<std::thread> workers_;
};

}  // namespace wsp

#endif  // WSP_GLOBAL_QUEUE_POOL_HPP
