#include "wsp/global_queue_pool.hpp"

#include <cassert>
#include <utility>

namespace wsp {

GlobalQueuePool::GlobalQueuePool(std::size_t workers) {
    if (workers == 0) {
        workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 1;  // hardware_concurrency may return 0
    }
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

GlobalQueuePool::~GlobalQueuePool() { shutdown(ShutdownMode::Drain); }

void GlobalQueuePool::shutdown(ShutdownMode mode) {
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        if (joined_) return;  // idempotent: already shut down
        accepting_.store(false, std::memory_order_release);  // pairs with acquire in enqueue()
        stop_ = true;
        if (mode == ShutdownMode::Cancel) cancel_ = true;
    }
    queue_cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    // Workers have joined, so the queue is now ours alone. Free any nodes that
    // were never executed: none on Drain (workers ran them all), the discarded
    // backlog on Cancel. Decrement pending_ per freed node so that a stray
    // wait() observes zero instead of hanging forever.
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        for (TaskNode* n = head_; n != nullptr;) {
            TaskNode* next = n->next;
            delete n;
            n = next;
            // acq_rel: keeps freed-node work before a concurrent wait()'s acquire load.
            pending_.fetch_sub(1, std::memory_order_acq_rel);
        }
        head_ = tail_ = nullptr;
        joined_ = true;
    }
    {
        std::lock_guard<std::mutex> lk(done_mtx_);
    }
    done_cv_.notify_all();
}

void GlobalQueuePool::enqueue(Task task) {
    if (!accepting_.load(std::memory_order_acquire)) return;  // shut down
    auto* node = new TaskNode(std::move(task));
    // Count the task before it becomes visible to a worker, otherwise a worker
    // could run and decrement pending_ before this increment lands, letting
    // wait() observe a spurious zero. Relaxed because the task is published under
    // queue_mtx_, whose lock/unlock already provides the necessary ordering.
    pending_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        node->next = nullptr;
        if (tail_ == nullptr) {
            head_ = tail_ = node;
        } else {
            tail_->next = node;
            tail_ = node;
        }
    }
    queue_cv_.notify_one();
}

void GlobalQueuePool::worker_loop() {
    for (;;) {
        TaskNode* node = nullptr;
        {
            std::unique_lock<std::mutex> lk(queue_mtx_);
            queue_cv_.wait(lk, [this] { return stop_ || head_ != nullptr; });
            if (cancel_ || head_ == nullptr) {
                // Cancel mode drops the queued backlog; otherwise we were woken
                // because the queue drained and we are shutting down.
                assert(stop_);
                return;
            }
            node = head_;
            head_ = head_->next;
            if (head_ == nullptr) tail_ = nullptr;
        }

        node->task();
        delete node;

        // Decrement after the task fully completes. If this was the last
        // outstanding task, wake any thread blocked in wait(). acq_rel pairs
        // with the load in wait() so the completed work happens-before wait()
        // returns.
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(done_mtx_);
            done_cv_.notify_all();
        }
    }
}

void GlobalQueuePool::wait() {
    std::unique_lock<std::mutex> lk(done_mtx_);
    done_cv_.wait(lk, [this] {
        return pending_.load(std::memory_order_acquire) == 0;
    });
}

}  // namespace wsp
