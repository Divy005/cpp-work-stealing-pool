// Evaluation / "inference" harness.
//
// Runs a small matrix of workload patterns against both the Phase 0 baseline and
// the Phase 1 work-stealing pool and reports, for each:
//
//   * throughput (Mtasks/s), median over several runs
//   * task scheduling latency percentiles p50 / p99 / p99.9 (microseconds),
//     i.e. how long a submitted task waits before a worker starts running it.
//
// Latency is the gap between enqueue() and the start of execution. Each task
// writes its own latency slot (indexed by task id) so the measurement itself is
// contention-free. Multiple runs + median make the numbers robust to the noise
// of a shared machine.
//
// Usage:
//   wsp_eval [--workers N] [--tasks N] [--runs N] [--pool global|ws|both]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "wsp/global_queue_pool.hpp"
#include "wsp/thread_pool.hpp"
#include "wsp/work_stealing_pool.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline double to_us(Clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}

inline void do_work(int iters) {
    volatile int x = 0;
    for (int i = 0; i < iters; ++i) x += i * 7 + 1;
}

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    const std::size_t idx = static_cast<std::size_t>(
        std::min(v.size() - 1, static_cast<std::size_t>(p * v.size())));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

struct Result {
    double throughput_mps = 0;  // Mtasks/s
    double p50 = 0, p99 = 0, p999 = 0;  // microseconds
};

// Factory so each run gets a fresh pool (clean worker state, fair timing).
using PoolFactory = std::function<std::unique_ptr<wsp::ThreadPool>()>;

// One run: submit n_tasks from `producers` threads, each task does work_iters of
// CPU work, and records its own scheduling latency. When `skewed` is set, ~1/16
// of the tasks do 16x the work, modelling an uneven load distribution that only
// balances well if idle workers steal the backlog off the busy deques.
Result one_run(wsp::ThreadPool& pool, std::size_t n, int producers,
               int work_iters, bool skewed) {
    std::vector<double> latency(n, 0.0);
    std::atomic<std::size_t> done{0};

    const auto t0 = Clock::now();
    std::vector<std::thread> ps;
    ps.reserve(static_cast<std::size_t>(producers));
    for (int p = 0; p < producers; ++p) {
        ps.emplace_back([&, p] {
            for (std::size_t i = static_cast<std::size_t>(p); i < n;
                 i += static_cast<std::size_t>(producers)) {
                const auto submit = Clock::now();
                pool.enqueue([&, i, submit, work_iters, skewed] {
                    latency[i] = to_us(Clock::now() - submit);
                    int w = work_iters;
                    if (skewed && (i % 16 == 0)) w *= 16;  // a few fat tasks
                    do_work(w);
                    done.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    for (auto& t : ps) t.join();
    pool.wait();
    const auto t1 = Clock::now();

    if (done.load() != n) {
        std::fprintf(stderr, "ERROR: %zu/%zu tasks ran\n", done.load(), n);
    }

    Result r;
    r.throughput_mps = (n / (to_us(t1 - t0) / 1e6)) / 1e6;
    r.p50 = percentile(latency, 0.50);
    r.p99 = percentile(latency, 0.99);
    r.p999 = percentile(latency, 0.999);
    return r;
}

Result median_of_runs(const PoolFactory& make, std::size_t n, int producers,
                      int work_iters, bool skewed, int runs) {
    // One warmup run (touch pages, spin up threads) then `runs` measured runs.
    { auto p = make(); (void)one_run(*p, n, producers, work_iters, skewed); }

    std::vector<Result> rs;
    rs.reserve(static_cast<std::size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        auto p = make();
        rs.push_back(one_run(*p, n, producers, work_iters, skewed));
    }
    auto med_by = [&](double Result::*field) {
        std::vector<double> xs;
        for (auto& r : rs) xs.push_back(r.*field);
        std::sort(xs.begin(), xs.end());
        return xs[xs.size() / 2];
    };
    Result m;
    m.throughput_mps = med_by(&Result::throughput_mps);
    m.p50 = med_by(&Result::p50);
    m.p99 = med_by(&Result::p99);
    m.p999 = med_by(&Result::p999);
    return m;
}

struct Workload {
    const char* name;
    std::size_t tasks;
    int producers;
    int work_iters;
    bool skewed;
};

void run_matrix(std::size_t workers, std::size_t tasks, int runs,
                bool do_global, bool do_ws) {
    PoolFactory make_global = [workers] {
        return std::unique_ptr<wsp::ThreadPool>(
            new wsp::GlobalQueuePool(workers));
    };
    PoolFactory make_ws = [workers] {
        return std::unique_ptr<wsp::ThreadPool>(
            new wsp::WorkStealingPool(workers));
    };

    const Workload workloads[] = {
        {"uniform     (1 producer, trivial)", tasks, 1, 0, false},
        {"contended   (4 producers, trivial)", tasks, 4, 0, false},
        {"prod/cons   (4 producers, ~300ns)", tasks, 4, 64, false},
        {"sustained   (2 producers, ~600ns)", tasks, 2, 128, false},
        {"bursty      (8 producers, ~150ns)", tasks, 8, 32, false},
        {"skewed      (4 producers, 1/16 fat)", tasks, 4, 16, true},
    };

    std::printf("workers=%zu tasks=%zu runs=%d (median reported)\n\n",
                workers, tasks, runs);
    std::printf("%-36s %-14s %10s %8s %8s %8s\n", "workload", "pool",
                "Mtasks/s", "p50(us)", "p99(us)", "p99.9");
    std::printf("%s\n", std::string(94, '-').c_str());

    for (const auto& w : workloads) {
        Result g{}, s{};
        if (do_global) {
            g = median_of_runs(make_global, w.tasks, w.producers, w.work_iters,
                               w.skewed, runs);
            std::printf("%-36s %-14s %10.3f %8.2f %8.2f %8.2f\n", w.name,
                        "global-queue", g.throughput_mps, g.p50, g.p99, g.p999);
        }
        if (do_ws) {
            s = median_of_runs(make_ws, w.tasks, w.producers, w.work_iters,
                               w.skewed, runs);
            std::printf("%-36s %-14s %10.3f %8.2f %8.2f %8.2f", w.name,
                        "work-stealing", s.throughput_mps, s.p50, s.p99, s.p999);
            if (do_global && g.throughput_mps > 0) {
                std::printf("   (%.2fx)", s.throughput_mps / g.throughput_mps);
            }
            std::printf("\n");
        }
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t workers = 0;  // 0 -> hardware_concurrency
    std::size_t tasks = 500'000;
    int runs = 3;
    bool do_global = true, do_ws = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if (a == "--workers") workers = std::strtoul(next(), nullptr, 10);
        else if (a == "--tasks") tasks = std::strtoul(next(), nullptr, 10);
        else if (a == "--runs") runs = std::atoi(next());
        else if (a == "--pool") {
            const std::string p = next();
            do_global = (p == "global" || p == "both");
            do_ws = (p == "ws" || p == "both");
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: %s [--workers N] [--tasks N] [--runs N] "
                "[--pool global|ws|both]\n",
                argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (workers == 0) {
        workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 1;
    }

    std::printf("=== Work-Stealing Thread Pool — Evaluation ===\n");
    run_matrix(workers, tasks, runs, do_global, do_ws);
    return 0;
}
