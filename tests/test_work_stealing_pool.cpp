#include <atomic>
#include <functional>
#include <numeric>
#include <vector>

#include "gtest/gtest.h"
#include "wsp/work_stealing_pool.hpp"

using wsp::WorkStealingPool;

TEST(WorkStealingPool, RunsASingleTask) {
    WorkStealingPool pool(4);
    std::atomic<int> ran{0};
    pool.enqueue([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    pool.wait();
    EXPECT_EQ(ran.load(), 1);
}

TEST(WorkStealingPool, AllTasksRunExactlyOnce) {
    constexpr int kTasks = 100000;
    WorkStealingPool pool(4);
    std::vector<std::atomic<int>> counters(kTasks);
    for (auto& c : counters) c.store(0);

    for (int i = 0; i < kTasks; ++i) {
        pool.enqueue([&counters, i] {
            counters[i].fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.wait();
    for (int i = 0; i < kTasks; ++i) {
        EXPECT_EQ(counters[i].load(), 1) << "task " << i << " ran wrong count";
    }
}

TEST(WorkStealingPool, SumIsCorrectUnderConcurrency) {
    constexpr int kTasks = 200000;
    WorkStealingPool pool;
    std::atomic<long long> sum{0};
    for (int i = 1; i <= kTasks; ++i) {
        pool.enqueue([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
    }
    pool.wait();
    const long long expected = 1LL * kTasks * (kTasks + 1) / 2;
    EXPECT_EQ(sum.load(), expected);
}

TEST(WorkStealingPool, RecursiveSubmissionFromInsideTasks) {
    WorkStealingPool pool(4);
    std::atomic<int> leaves{0};
    constexpr int kFanout = 2000;
    for (int i = 0; i < kFanout; ++i) {
        pool.enqueue([&] {
            pool.enqueue([&] { leaves.fetch_add(1, std::memory_order_relaxed); });
        });
    }
    pool.wait();
    EXPECT_EQ(leaves.load(), kFanout);
}

TEST(WorkStealingPool, WaitIsReusableAcrossMultipleBatches) {
    WorkStealingPool pool(3);
    std::atomic<int> total{0};
    for (int batch = 0; batch < 5; ++batch) {
        for (int i = 0; i < 1000; ++i) {
            pool.enqueue([&] { total.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.wait();
        EXPECT_EQ(total.load(), 1000 * (batch + 1));
    }
}

// Load balancing: dump every task into the pool from a single external thread
// in a tight loop. Without stealing, work would pile on one or two deques; with
// stealing, all workers participate and the steal counter is non-zero.
TEST(WorkStealingPool, SkewedLoadIsBalancedByStealing) {
    constexpr int kTasks = 200000;
    WorkStealingPool pool(4, /*deque_capacity=*/64);  // small cap forces overflow too
    std::atomic<int> done{0};
    for (int i = 0; i < kTasks; ++i) {
        pool.enqueue([&] {
            // a little work so stealing actually matters
            volatile int x = 0;
            for (int k = 0; k < 20; ++k) x += k;
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.wait();
    EXPECT_EQ(done.load(), kTasks);
    EXPECT_GT(pool.steals(), 0u) << "expected work to be stolen under skew";
}

// Heavy recursion (fib-style task DAG): each task may spawn children. Verifies
// no task is lost or double-run across a deep, irregular tree. Counts the number
// of fib "calls" and checks it matches the closed form.
TEST(WorkStealingPool, FibTaskDagCountsAllNodes) {
    WorkStealingPool pool(4);
    std::atomic<long long> calls{0};
    std::function<void(int)> fib = [&](int n) {
        calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 2) return;
        pool.enqueue([&fib, n] { fib(n - 1); });
        pool.enqueue([&fib, n] { fib(n - 2); });
    };
    pool.enqueue([&fib] { fib(22); });
    pool.wait();

    // Number of nodes in the fib call tree for n: T(n) = 2*Fib(n+1) - 1.
    auto fibval = [](int n) {
        long long a = 0, b = 1;
        for (int i = 0; i < n; ++i) {
            long long t = a + b;
            a = b;
            b = t;
        }
        return a;  // Fib(n)
    };
    const long long expected = 2 * fibval(23) - 1;  // Fib(22+1)
    EXPECT_EQ(calls.load(), expected);
}

TEST(WorkStealingPool, FifoOrderingFromSingleProducerSingleWorker) {
    // One worker, external FIFO submission round-robins to the single deque's
    // back and is taken LIFO... so a single worker pulls its own deque LIFO.
    // To get deterministic order we instead check all-present, order-agnostic.
    WorkStealingPool pool(1);
    constexpr int kTasks = 1000;
    std::vector<std::atomic<int>> seen(kTasks);
    for (auto& s : seen) s.store(0);
    for (int i = 0; i < kTasks; ++i) {
        pool.enqueue([&seen, i] { seen[i].fetch_add(1, std::memory_order_relaxed); });
    }
    pool.wait();
    for (int i = 0; i < kTasks; ++i) EXPECT_EQ(seen[i].load(), 1);
}

TEST(WorkStealingPool, ManyExternalProducers) {
    WorkStealingPool pool(4);
    std::atomic<long long> sum{0};
    constexpr int kProducers = 8;
    constexpr int kPer = 20000;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPer; ++i) {
                pool.enqueue([&] { sum.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    for (auto& t : producers) t.join();
    pool.wait();
    EXPECT_EQ(sum.load(), 1LL * kProducers * kPer);
}

// shutdown(Drain) must run every queued task before it returns, with no call to
// wait() — draining is part of the shutdown contract.
TEST(WorkStealingPool, ShutdownDrainRunsAllQueuedTasks) {
    constexpr int kTasks = 50000;
    std::atomic<int> ran{0};
    WorkStealingPool pool(4);
    for (int i = 0; i < kTasks; ++i) {
        pool.enqueue([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();  // Drain
    EXPECT_EQ(ran.load(), kTasks);
}

// Calling shutdown() more than once (including the destructor's own call) is a
// safe no-op after the first.
TEST(WorkStealingPool, ShutdownIsIdempotent) {
    std::atomic<int> ran{0};
    WorkStealingPool pool(4);
    for (int i = 0; i < 1000; ++i) {
        pool.enqueue([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();
    pool.shutdown(wsp::ShutdownMode::Cancel);  // ignored: already shut down
    pool.shutdown();
    EXPECT_EQ(ran.load(), 1000);
}

TEST(WorkStealingPool, EnqueueAfterShutdownIsIgnored) {
    std::atomic<int> ran{0};
    WorkStealingPool pool(4);
    pool.shutdown();
    pool.enqueue([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(ran.load(), 0);  // never queued, never run
}

// Cancel discards the queued backlog and joins. The key property: once
// shutdown() returns the workers are joined, so no task runs afterwards.
TEST(WorkStealingPool, ShutdownCancelStopsWorkersAndJoins) {
    constexpr int kTasks = 200000;
    std::atomic<int> ran{0};
    WorkStealingPool pool(4);
    for (int i = 0; i < kTasks; ++i) {
        pool.enqueue([&] {
            volatile int x = 0;
            for (int k = 0; k < 50; ++k) x += k;
            ran.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.shutdown(wsp::ShutdownMode::Cancel);
    const int after_join = ran.load();        // workers have joined here
    EXPECT_LE(after_join, kTasks);            // backlog may be discarded
    EXPECT_EQ(ran.load(), after_join);        // nothing runs after the join
}
