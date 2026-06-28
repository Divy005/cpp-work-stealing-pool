#include <atomic>
#include <numeric>
#include <vector>

#include "gtest/gtest.h"
#include "wsp/global_queue_pool.hpp"

using wsp::GlobalQueuePool;

TEST(GlobalQueuePool, RunsASingleTask) {
    GlobalQueuePool pool(4);
    std::atomic<int> ran{0};
    pool.enqueue([&] { ran.fetch_add(1, std::memory_order_relaxed); });
    pool.wait();
    EXPECT_EQ(ran.load(), 1);
}

TEST(GlobalQueuePool, AllTasksRunExactlyOnce) {
    constexpr int kTasks = 100000;
    GlobalQueuePool pool(4);
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

TEST(GlobalQueuePool, SumIsCorrectUnderConcurrency) {
    constexpr int kTasks = 200000;
    GlobalQueuePool pool;  // default = hardware_concurrency
    std::atomic<long long> sum{0};
    for (int i = 1; i <= kTasks; ++i) {
        pool.enqueue([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); });
    }
    pool.wait();
    const long long expected = 1LL * kTasks * (kTasks + 1) / 2;
    EXPECT_EQ(sum.load(), expected);
}

TEST(GlobalQueuePool, RecursiveSubmissionFromInsideTasks) {
    // Tasks that spawn more tasks; wait() must account for the whole tree.
    GlobalQueuePool pool(4);
    std::atomic<int> leaves{0};
    constexpr int kFanout = 1000;
    for (int i = 0; i < kFanout; ++i) {
        pool.enqueue([&] {
            pool.enqueue([&] { leaves.fetch_add(1, std::memory_order_relaxed); });
        });
    }
    pool.wait();
    EXPECT_EQ(leaves.load(), kFanout);
}

TEST(GlobalQueuePool, WaitIsReusableAcrossMultipleBatches) {
    GlobalQueuePool pool(3);
    std::atomic<int> total{0};
    for (int batch = 0; batch < 5; ++batch) {
        for (int i = 0; i < 1000; ++i) {
            pool.enqueue([&] { total.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.wait();
        EXPECT_EQ(total.load(), 1000 * (batch + 1));
    }
}

TEST(GlobalQueuePool, FifoOrderingFromSingleProducerSingleWorker) {
    // With one worker and a FIFO queue, tasks execute in submission order.
    GlobalQueuePool pool(1);
    std::vector<int> order;
    for (int i = 0; i < 1000; ++i) {
        pool.enqueue([&order, i] { order.push_back(i); });
    }
    pool.wait();
    std::vector<int> expected(1000);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(order, expected);
}
