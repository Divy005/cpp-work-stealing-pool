#ifndef WSP_TASK_HPP
#define WSP_TASK_HPP

#include <functional>
#include <utility>

namespace wsp {

// A Task is the unit of work scheduled by the pool. We type-erase the callable
// behind std::function<void()> so any nullary invocable (lambda, bound member,
// free function) can be submitted. Arguments are captured by the caller.
using Task = std::function<void()>;

// TaskNode is the *intrusive* node used by the per-worker deque in Phase 1.
//
// "Intrusive" means the linked-list pointers live inside the node that owns the
// payload, rather than in a separate container node. This keeps the deque
// allocation-light on the hot path: enqueuing a task is a single `new TaskNode`
// plus pointer splicing, and dequeuing is pointer splicing plus `delete`.
//
// Ownership contract:
//   * A node is created by enqueue() and handed to exactly one queue.
//   * Exactly one worker pops it (from the back as owner, or from the front as
//     a thief) and is then responsible for running it and deleting it.
//   * `prev`/`next` are only ever touched while the owning deque's lock is held
//     (or, for the not-yet-linked node, by its sole creator before insertion).
struct TaskNode {
    Task task;
    TaskNode* prev = nullptr;  // toward the front (steal end)
    TaskNode* next = nullptr;  // toward the back  (owner push/pop end)

    explicit TaskNode(Task t) : task(std::move(t)) {}
};

}  // namespace wsp

#endif  // WSP_TASK_HPP
