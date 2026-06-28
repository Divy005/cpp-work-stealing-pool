// Custom micro-benchmark harness.
//
// Submits a large number of trivial tasks through each pool and measures
// end-to-end throughput (tasks/second). Used to quantify the Phase 1
// work-stealing speedup over the Phase 0 global-queue baseline. Written against
// the ThreadPool interface so any pool can be measured the same way.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "wsp/global_queue_pool.hpp"
#include "wsp/thread_pool.hpp"
#include "wsp/work_stealing_pool.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// A few hundred ns of CPU work, opaque to the optimizer. Models a non-trivial
// task so per-task scheduling overhead is amortized and parallelism shows.
inline void do_work(int iters) {
    volatile int x = 0;
    for (int i = 0; i < iters; ++i) x += i * 7 + 1;
}

// Flat workload: N independent tasks submitted by `producers` threads.
double bench_flat(wsp::ThreadPool& pool, std::size_t n_tasks, int producers,
                  int work_iters) {
    std::atomic<std::size_t> counter{0};
    const auto start = Clock::now();
    std::vector<std::thread> ps;
    for (int p = 0; p < producers; ++p) {
        ps.emplace_back([&, p] {
            for (std::size_t i = p; i < n_tasks;
                 i += static_cast<std::size_t>(producers)) {
                pool.enqueue([&counter, work_iters] {
                    do_work(work_iters);
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    for (auto& t : ps) t.join();
    pool.wait();
    const auto end = Clock::now();

    if (counter.load() != n_tasks) {
        std::fprintf(stderr, "ERROR: ran %zu / %zu tasks\n", counter.load(),
                     n_tasks);
    }
    const double secs = std::chrono::duration<double>(end - start).count();
    return n_tasks / secs;
}

// Recursive fan-out (work-stealing's home turf): a divide-and-conquer tree where
// each node spawns children onto the running worker's own deque.
double bench_fanout(wsp::ThreadPool& pool, int depth, std::atomic<long long>& leaves) {
    leaves.store(0);
    std::function<void(int)> spawn = [&](int d) {
        if (d == 0) {
            leaves.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pool.enqueue([&spawn, d] { spawn(d - 1); });
        pool.enqueue([&spawn, d] { spawn(d - 1); });
    };
    const auto start = Clock::now();
    pool.enqueue([&spawn, depth] { spawn(depth); });
    pool.wait();
    const auto end = Clock::now();
    const double secs = std::chrono::duration<double>(end - start).count();
    return (1LL << depth) / secs;  // leaves per second
}

}  // namespace

// Runs both pools on a flat workload and prints the speedup.
void compare_flat(const char* title, std::size_t n, int producers, int work) {
    std::printf("=== %s ===\n", title);
    double base = 0.0, ws = 0.0;
    {
        wsp::GlobalQueuePool pool;
        base = bench_flat(pool, n, producers, work);
        std::printf("%-14s workers=%zu  %.3f Mtasks/s\n", pool.name(),
                    pool.worker_count(), base / 1e6);
    }
    {
        wsp::WorkStealingPool pool;
        ws = bench_flat(pool, n, producers, work);
        std::printf("%-14s workers=%zu  %.3f Mtasks/s  steals=%llu overflow=%llu\n",
                    pool.name(), pool.worker_count(), ws / 1e6,
                    (unsigned long long)pool.steals(),
                    (unsigned long long)pool.overflow_pushes());
    }
    std::printf("  -> work-stealing speedup: %.2fx\n\n", ws / base);
}

int main() {
    constexpr std::size_t kFlat = 1'000'000;
    constexpr int kDepth = 20;  // 2^20 ~= 1.05M leaf tasks

    compare_flat("Flat, 1 producer, trivial tasks (producer-bound)", kFlat, 1, 0);
    compare_flat("Flat, 4 producers, trivial tasks", kFlat, 4, 0);
    compare_flat("Flat, 4 producers, ~300ns tasks", kFlat, 4, 64);

    std::printf("=== Recursive fan-out: 2^%d leaf tasks ===\n", kDepth);
    std::atomic<long long> leaves{0};
    double base_fan = 0.0, ws_fan = 0.0;
    {
        wsp::GlobalQueuePool pool;
        base_fan = bench_fanout(pool, kDepth, leaves);
        std::printf("%-14s workers=%zu  %.3f Mtasks/s\n", pool.name(),
                    pool.worker_count(), base_fan / 1e6);
    }
    {
        wsp::WorkStealingPool pool;
        ws_fan = bench_fanout(pool, kDepth, leaves);
        std::printf("%-14s workers=%zu  %.3f Mtasks/s  steals=%llu\n",
                    pool.name(), pool.worker_count(), ws_fan / 1e6,
                    (unsigned long long)pool.steals());
    }
    std::printf("  -> work-stealing speedup: %.2fx\n", ws_fan / base_fan);
    return 0;
}
