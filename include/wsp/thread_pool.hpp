#ifndef WSP_THREAD_POOL_HPP
#define WSP_THREAD_POOL_HPP

#include <cstddef>

#include "wsp/task.hpp"

namespace wsp {

// Common interface implemented by every scheduler in this project. Having a
// shared interface lets the test suite and the evaluation harness run the exact
// same workloads against the Phase 0 global-queue pool and the Phase 1
// work-stealing pool, which is how we quantify the speedup.
class ThreadPool {
public:
    virtual ~ThreadPool() = default;

    // Submit a task for execution. Thread-safe; callable from any thread,
    // including from inside a running task (recursive submission).
    virtual void enqueue(Task task) = 0;

    // Block until every task submitted so far (and any tasks they spawned) has
    // finished executing. Does not shut the pool down; more work may follow.
    virtual void wait() = 0;

    // Number of worker threads.
    virtual std::size_t worker_count() const = 0;

    // Human-readable name, used in benchmark/eval output.
    virtual const char* name() const = 0;
};

}  // namespace wsp

#endif  // WSP_THREAD_POOL_HPP
