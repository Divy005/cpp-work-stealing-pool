// Unit tests for the hand-rolled intrusive deque in isolation: link integrity,
// the two ends (owner LIFO at the back, thief FIFO at the front), capacity, and
// the single-element edge case where pop_back and steal_front collide.

#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "wsp/intrusive_deque.hpp"

using wsp::IntrusiveDeque;
using wsp::TaskNode;

namespace {
TaskNode* make_node(int value, int* sink) {
    return new TaskNode([sink, value] { *sink = value; });
}
}  // namespace

TEST(IntrusiveDeque, EmptyDequeReturnsNull) {
    IntrusiveDeque d;
    EXPECT_TRUE(d.empty());
    EXPECT_EQ(d.pop_back(), nullptr);
    EXPECT_EQ(d.steal_front(), nullptr);
    d.validate();
}

TEST(IntrusiveDeque, OwnerEndIsLifo) {
    IntrusiveDeque d;
    int sink = 0;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(d.try_push_back(make_node(i, &sink)));
    d.validate();
    EXPECT_EQ(d.size(), 5u);
    // pop_back returns most-recently-pushed first: 4,3,2,1,0
    for (int expected = 4; expected >= 0; --expected) {
        TaskNode* n = d.pop_back();
        ASSERT_NE(n, nullptr);
        n->task();
        EXPECT_EQ(sink, expected);
        delete n;
    }
    EXPECT_TRUE(d.empty());
    d.validate();
}

TEST(IntrusiveDeque, ThiefEndIsFifo) {
    IntrusiveDeque d;
    int sink = 0;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(d.try_push_back(make_node(i, &sink)));
    // steal_front returns oldest first: 0,1,2,3,4
    for (int expected = 0; expected < 5; ++expected) {
        TaskNode* n = d.steal_front();
        ASSERT_NE(n, nullptr);
        n->task();
        EXPECT_EQ(sink, expected);
        delete n;
    }
    EXPECT_TRUE(d.empty());
    d.validate();
}

TEST(IntrusiveDeque, SingleElementGoesToExactlyOneEnd) {
    int sink = 0;
    {
        IntrusiveDeque d;
        ASSERT_TRUE(d.try_push_back(make_node(42, &sink)));
        TaskNode* a = d.pop_back();
        TaskNode* b = d.steal_front();
        // Exactly one of them gets the node; the other gets null.
        EXPECT_TRUE((a != nullptr) ^ (b != nullptr));
        delete a;
        delete b;
        EXPECT_TRUE(d.empty());
        d.validate();
    }
}

TEST(IntrusiveDeque, RespectsCapacity) {
    IntrusiveDeque d(3);
    int sink = 0;
    EXPECT_TRUE(d.try_push_back(make_node(0, &sink)));
    EXPECT_TRUE(d.try_push_back(make_node(1, &sink)));
    EXPECT_TRUE(d.try_push_back(make_node(2, &sink)));
    TaskNode* overflow = make_node(3, &sink);
    EXPECT_FALSE(d.try_push_back(overflow));  // full -> rejected, not owned
    delete overflow;                          // caller still owns it
    EXPECT_EQ(d.size(), 3u);
    d.validate();
}

TEST(IntrusiveDeque, MixedPushPopKeepsLinksConsistent) {
    IntrusiveDeque d;
    int sink = 0;
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(d.try_push_back(make_node(i, &sink)));
    delete d.pop_back();
    delete d.steal_front();
    delete d.pop_back();
    delete d.steal_front();
    d.validate();
    EXPECT_EQ(d.size(), 6u);
    // Drain remainder.
    while (TaskNode* n = d.pop_back()) delete n;
    EXPECT_TRUE(d.empty());
    d.validate();
}

// Concurrency: one owner pushing/popping the back while many thieves steal from
// the front. Every node must be taken exactly once (no loss, no double-take).
// Run under TSan this also asserts the per-deque lock makes the deque race-free.
TEST(IntrusiveDeque, ConcurrentOwnerAndThievesTakeEachNodeOnce) {
    constexpr int kNodes = 50000;
    IntrusiveDeque d;
    std::atomic<int> taken{0};
    std::atomic<bool> producing{true};

    auto consume = [&](TaskNode* n) {
        if (n) {
            taken.fetch_add(1, std::memory_order_relaxed);
            delete n;
        }
    };

    std::vector<std::thread> thieves;
    for (int t = 0; t < 3; ++t) {
        thieves.emplace_back([&] {
            while (producing.load(std::memory_order_acquire) ||
                   !d.empty()) {
                consume(d.steal_front());
            }
        });
    }

    int sink = 0;
    for (int i = 0; i < kNodes; ++i) {
        // Owner interleaves its own pop_back with pushes (LIFO self-work).
        ASSERT_TRUE(d.try_push_back(make_node(i, &sink)));
        if ((i & 7) == 0) consume(d.pop_back());
    }
    producing.store(false, std::memory_order_release);

    for (auto& th : thieves) th.join();
    // Owner mops up anything left.
    while (TaskNode* n = d.pop_back()) consume(n);

    EXPECT_EQ(taken.load(), kNodes);
    EXPECT_TRUE(d.empty());
    d.validate();
}
