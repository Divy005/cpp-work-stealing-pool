#ifndef WSP_INTRUSIVE_DEQUE_HPP
#define WSP_INTRUSIVE_DEQUE_HPP

#include <cassert>
#include <cstddef>
#include <mutex>

#include "wsp/task.hpp"

namespace wsp {

// Hand-rolled per-worker work-stealing deque.
//
// Storage is an *intrusive* doubly-linked list of TaskNode (the list pointers
// live in the node, so no separate container allocation). The deque has two
// ends:
//
//     front_ <-> n <-> n <-> ... <-> back_
//       ^ steal end (thieves)          ^ owner end (push/pop)
//
// Concurrency model (Phase 1: fine-grained locking — one mutex *per deque*):
//   * The OWNER thread uses the back: try_push_back / pop_back (LIFO, so the
//     most-recently-pushed, cache-hot task is taken first).
//   * A THIEF uses the front: steal_front (FIFO, takes the oldest task).
//   * Every operation takes this deque's mutex. Because each worker has its own
//     deque/lock, owner operations are almost always uncontended, and thieves
//     spread across N independent locks instead of one global lock.
//
// Invariants (checked by assert / validate()):
//   I1. front_ == nullptr  <=>  back_ == nullptr  <=>  size_ == 0
//   I2. front_->prev == nullptr ; back_->next == nullptr ; and for every
//       interior link, n->next->prev == n and n->prev->next == n.
//   I3. A node belongs to exactly one deque at a time (single ownership), so it
//       is never executed twice and never dropped.
//   I4. prev/next are only touched while mtx_ is held.
class IntrusiveDeque {
public:
    // capacity == 0 means unbounded.
    explicit IntrusiveDeque(std::size_t capacity = 0) : capacity_(capacity) {}

    ~IntrusiveDeque() {
        // Free any nodes left undrained (e.g. abrupt shutdown). We delete; we
        // do not run them.
        for (TaskNode* n = front_; n != nullptr;) {
            TaskNode* next = n->next;
            delete n;
            n = next;
        }
    }

    IntrusiveDeque(const IntrusiveDeque&) = delete;
    IntrusiveDeque& operator=(const IntrusiveDeque&) = delete;

    // OWNER: push a node onto the back. Returns false (without taking ownership)
    // if the deque is at capacity, so the caller can route to the overflow
    // queue. On success the deque takes ownership of `node`.
    bool try_push_back(TaskNode* node) {
        assert(node != nullptr);
        std::lock_guard<std::mutex> lk(mtx_);
        if (capacity_ != 0 && size_ >= capacity_) return false;
        node->next = nullptr;
        node->prev = back_;
        if (back_ == nullptr) {
            front_ = back_ = node;
        } else {
            back_->next = node;
            back_ = node;
        }
        ++size_;
        return true;
    }

    // OWNER: pop the most-recently-pushed node from the back (LIFO). Returns
    // nullptr if empty.
    TaskNode* pop_back() {
        std::lock_guard<std::mutex> lk(mtx_);
        TaskNode* node = back_;
        if (node == nullptr) return nullptr;
        back_ = node->prev;
        if (back_ == nullptr) {
            front_ = nullptr;  // became empty
        } else {
            back_->next = nullptr;
        }
        --size_;
        node->prev = node->next = nullptr;  // detach (I3)
        return node;
    }

    // THIEF: steal the oldest node from the front (FIFO). Returns nullptr if
    // empty. Correct even when the deque has a single element — the lock makes
    // pop_back and steal_front mutually exclusive on that last node.
    TaskNode* steal_front() {
        std::lock_guard<std::mutex> lk(mtx_);
        TaskNode* node = front_;
        if (node == nullptr) return nullptr;
        front_ = node->next;
        if (front_ == nullptr) {
            back_ = nullptr;  // became empty
        } else {
            front_->prev = nullptr;
        }
        --size_;
        node->prev = node->next = nullptr;  // detach (I3)
        return node;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return size_;
    }

    bool empty() const { return size() == 0; }

    // Test/debug helper: assert all structural invariants hold.
    void validate() const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (size_ == 0) {
            assert(front_ == nullptr && back_ == nullptr);
            return;
        }
        assert(front_ != nullptr && back_ != nullptr);
        assert(front_->prev == nullptr);
        assert(back_->next == nullptr);
        std::size_t count = 0;
        for (TaskNode* n = front_; n != nullptr; n = n->next) {
            ++count;
            if (n->next) assert(n->next->prev == n);
            if (n->prev) assert(n->prev->next == n);
        }
        assert(count == size_);
    }

private:
    mutable std::mutex mtx_;
    TaskNode* front_ = nullptr;
    TaskNode* back_ = nullptr;
    std::size_t size_ = 0;
    const std::size_t capacity_;
};

}  // namespace wsp

#endif  // WSP_INTRUSIVE_DEQUE_HPP
