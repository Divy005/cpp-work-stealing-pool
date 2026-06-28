// Custom micro-benchmark harness.
//
// Phase 0 scope: submit a large number of trivial tasks through the pool and
// measure end-to-end throughput (tasks/second). This establishes the baseline
// that the Phase 1 work-stealing pool must beat. The harness is written against
// the ThreadPool interface so later phases can drop in other pools unchanged.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

#include "wsp/global_queue_pool.hpp"
#include "wsp/thread_pool.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Flat workload: N independent trivial tasks submitted from the main thread.
double bench_flat(wsp::ThreadPool& pool, std::size_t n_tasks) {
    std::atomic<std::size_t> counter{0};
    const auto start = Clock::now();
    for (std::size_t i = 0; i < n_tasks; ++i) {
        pool.enqueue([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.wait();
    const auto end = Clock::now();

    if (counter.load() != n_tasks) {
        std::fprintf(stderr, "ERROR: ran %zu / %zu tasks\n", counter.load(),
                     n_tasks);
    }
    const double secs = std::chrono::duration<double>(end - start).count();
    return n_tasks / secs;
}

}  // namespace

int main() {
    constexpr std::size_t kTasks = 1'000'000;

    std::printf("=== Phase 0 benchmark: flat workload, %zu tasks ===\n", kTasks);
    {
        wsp::GlobalQueuePool pool;
        const double tput = bench_flat(pool, kTasks);
        std::printf("%-16s workers=%zu  throughput=%.2f Mtasks/s\n", pool.name(),
                    pool.worker_count(), tput / 1e6);
    }
    return 0;
}
