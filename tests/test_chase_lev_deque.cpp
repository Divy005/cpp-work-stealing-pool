// Tests for the lock-free Chase-Lev deque: the two ends (owner LIFO at the
// bottom, thief FIFO at the top), the full/empty edges, the single-element
// pop-vs-steal tie, and an exact-count owner-vs-thieves stress test that catches
// any lost or duplicated node. The stress test is the functional stand-in for
// ThreadSanitizer on the Windows dev box; under TSan on Linux it also proves the
// memory ordering race-free.

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "wsp/chase_lev_deque.hpp"

using wsp::ChaseLevDeque;
using wsp::TaskNode;

namespace {
TaskNode* make_node(int value, int* sink) {
    return new TaskNode([sink, value] { *sink = value; });
}
}  // namespace

TEST(ChaseLevDeque, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(ChaseLevDeque(1000).capacity(), 1024u);
    EXPECT_EQ(ChaseLevDeque(1024).capacity(), 1024u);
    EXPECT_EQ(ChaseLevDeque(0).capacity(), 2u);
}

TEST(ChaseLevDeque, EmptyReturnsNull) {
    ChaseLevDeque d(8);
    EXPECT_TRUE(d.empty());
    EXPECT_EQ(d.pop_bottom(), nullptr);
    EXPECT_EQ(d.steal_top(), nullptr);
}

TEST(ChaseLevDeque, OwnerBottomIsLifo) {
    ChaseLevDeque d(16);
    int sink = 0;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(d.push_bottom(make_node(i, &sink)));
    EXPECT_EQ(d.size(), 5u);
    for (int expected = 4; expected >= 0; --expected) {  // LIFO: 4,3,2,1,0
        TaskNode* n = d.pop_bottom();
        ASSERT_NE(n, nullptr);
        n->task();
        EXPECT_EQ(sink, expected);
        delete n;
    }
    EXPECT_TRUE(d.empty());
}

TEST(ChaseLevDeque, ThiefTopIsFifo) {
    ChaseLevDeque d(16);
    int sink = 0;
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(d.push_bottom(make_node(i, &sink)));
    for (int expected = 0; expected < 5; ++expected) {  // FIFO: 0,1,2,3,4
        TaskNode* n = d.steal_top();
        ASSERT_NE(n, nullptr);
        n->task();
        EXPECT_EQ(sink, expected);
        delete n;
    }
    EXPECT_TRUE(d.empty());
}

TEST(ChaseLevDeque, FullPushReturnsFalseWithoutTakingOwnership) {
    ChaseLevDeque d(4);  // capacity exactly 4
    int sink = 0;
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(d.push_bottom(make_node(i, &sink)));
    TaskNode* overflow = make_node(99, &sink);
    EXPECT_FALSE(d.push_bottom(overflow));  // full -> rejected
    delete overflow;                        // caller still owns it
    EXPECT_EQ(d.size(), 4u);
}

TEST(ChaseLevDeque, SingleElementGoesToExactlyOneEnd) {
    int sink = 0;
    ChaseLevDeque d(8);
    ASSERT_TRUE(d.push_bottom(make_node(42, &sink)));
    TaskNode* a = d.pop_bottom();
    TaskNode* b = d.steal_top();
    EXPECT_TRUE((a != nullptr) ^ (b != nullptr));  // exactly one wins
    delete a;
    delete b;
    EXPECT_TRUE(d.empty());
}

// The crucial test: one owner (push/pop the bottom) racing many thieves (steal
// the top). Every node must be taken exactly once -- no loss, no double-take.
// A double-take would double-free a node (caught by ASan / a crash); a loss
// shows up as taken < kNodes. Run under TSan this also asserts race-freedom.
TEST(ChaseLevDeque, ConcurrentOwnerAndThievesTakeEachNodeOnce) {
    constexpr int kNodes = 200000;
    ChaseLevDeque d(1024);
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
            while (producing.load(std::memory_order_acquire)) {
                consume(d.steal_top());
            }
        });
    }

    int sink = 0;
    for (int i = 0; i < kNodes; ++i) {
        TaskNode* n = make_node(i, &sink);
        // Push; if the ring is full, make room by popping our own bottom (those
        // popped nodes are taken by the owner and still counted exactly once).
        while (!d.push_bottom(n)) consume(d.pop_bottom());
        if ((i & 7) == 0) consume(d.pop_bottom());  // interleave owner self-pops
    }
    producing.store(false, std::memory_order_release);
    for (auto& th : thieves) th.join();
    // Thieves have stopped; the owner mops up whatever remains.
    while (TaskNode* n = d.pop_bottom()) consume(n);

    EXPECT_EQ(taken.load(), kNodes);
    EXPECT_TRUE(d.empty());
}
