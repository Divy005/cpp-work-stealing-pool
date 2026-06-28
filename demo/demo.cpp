// Demo / stress program: two real divide-and-conquer workloads driven by the
// work-stealing pool, each timed and verified for correctness.
//
//   1. Fib task DAG  — a deep, irregular recursive task tree (each call spawns
//      its two children as tasks). Exercises recursive submission and stealing.
//   2. Parallel sort — chunked parallel std::sort + pairwise merge, compared
//      against a single-threaded std::sort on the same data.
//
// Usage: wsp_demo [fib_n] [sort_n]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <vector>

#include "wsp/work_stealing_pool.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double secs_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

long long fib_closed(int n) {
    long long a = 0, b = 1;
    for (int i = 0; i < n; ++i) {
        const long long t = a + b;
        a = b;
        b = t;
    }
    return a;  // Fib(n)
}

// 1. Fib task DAG. The naive fib recursion as a task tree: each fib(k) spawns
// fib(k-1) and fib(k-2) as tasks, and base cases accumulate into a shared total.
// The sum of base-case values equals Fib(n), which we check against the closed
// form — so a lost or double-run task would show up as a wrong result.
void demo_fib(wsp::WorkStealingPool& pool, int n) {
    std::atomic<long long> total{0};
    std::function<void(int)> fib = [&](int k) {
        if (k < 2) {
            total.fetch_add(k, std::memory_order_relaxed);
            return;
        }
        pool.enqueue([&fib, k] { fib(k - 1); });
        pool.enqueue([&fib, k] { fib(k - 2); });
    };

    const std::uint64_t steals0 = pool.steals();
    const auto t0 = Clock::now();
    pool.enqueue([&fib, n] { fib(n); });
    pool.wait();
    const double s = secs_since(t0);

    const long long got = total.load();
    const long long expected = fib_closed(n);
    const double tasks = 2.0 * static_cast<double>(fib_closed(n + 1)) - 1.0;
    std::printf("fib DAG   n=%d  result=%lld expected=%lld  %s\n"
                "          ~%.0f tasks in %.3fs = %.2f Mtask/s  (steals=%llu)\n",
                n, got, expected, got == expected ? "OK" : "FAIL", tasks, s,
                tasks / s / 1e6,
                (unsigned long long)(pool.steals() - steals0));
}

// 2. Parallel pool-sort. Split the array into many contiguous chunks, sort each
// as a task, then merge the sorted runs pairwise. Verified against a
// single-threaded std::sort of the same input.
void demo_sort(wsp::WorkStealingPool& pool, std::size_t n) {
    std::mt19937 rng(123456789u);
    std::vector<int> data(n);
    for (auto& x : data) x = static_cast<int>(rng());
    std::vector<int> reference = data;

    const auto t_ref = Clock::now();
    std::sort(reference.begin(), reference.end());
    const double ref_s = secs_since(t_ref);

    // 8 chunks per worker gives the stealer enough independent pieces to balance.
    const std::size_t chunks = pool.worker_count() * 8;
    const std::size_t chunk = (n + chunks - 1) / chunks;

    const auto t_par = Clock::now();
    for (std::size_t start = 0; start < n; start += chunk) {
        const std::size_t lo = start;
        const std::size_t hi = std::min(n, start + chunk);
        pool.enqueue([&data, lo, hi] {
            std::sort(data.begin() + static_cast<std::ptrdiff_t>(lo),
                      data.begin() + static_cast<std::ptrdiff_t>(hi));
        });
    }
    pool.wait();  // all chunks now individually sorted

    // Merge adjacent sorted runs, doubling the run width each pass, until the
    // whole array is one sorted run. (Sequential tail; the sort phase above is
    // the parallel part.)
    for (std::size_t width = chunk; width < n; width *= 2) {
        for (std::size_t lo = 0; lo < n; lo += 2 * width) {
            const std::size_t mid = std::min(n, lo + width);
            const std::size_t hi = std::min(n, lo + 2 * width);
            std::inplace_merge(data.begin() + static_cast<std::ptrdiff_t>(lo),
                               data.begin() + static_cast<std::ptrdiff_t>(mid),
                               data.begin() + static_cast<std::ptrdiff_t>(hi));
        }
    }
    const double par_s = secs_since(t_par);

    const bool ok = (data == reference);
    std::printf("pool-sort n=%zu  %s  std::sort=%.3fs  pool=%.3fs  speedup=%.2fx"
                "  (chunks=%zu)\n",
                n, ok ? "OK" : "FAIL", ref_s, par_s,
                par_s > 0 ? ref_s / par_s : 0.0, chunks);
}

}  // namespace

int main(int argc, char** argv) {
    int fib_n = 30;
    std::size_t sort_n = 10'000'000;
    if (argc > 1) fib_n = std::atoi(argv[1]);
    if (argc > 2) sort_n = std::strtoull(argv[2], nullptr, 10);

    wsp::WorkStealingPool pool;
    std::printf("=== Work-Stealing Pool — Demo (workers=%zu) ===\n",
                pool.worker_count());
    demo_fib(pool, fib_n);
    demo_sort(pool, sort_n);
    return 0;
}
