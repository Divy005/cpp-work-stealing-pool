# DESIGN — Work-Stealing Thread Pool

This document is the living design record. It covers the implementation plan,
the data structures, the steal algorithm, the synchronization invariants, the
memory-ordering rationale, and the per-phase acceptance criteria.

---

## 1. Implementation plan & file layout

```
include/wsp/
  task.hpp               Task (std::function<void()>) + intrusive TaskNode
  thread_pool.hpp        ThreadPool interface (enqueue / wait / worker_count)
  global_queue_pool.hpp  Phase 0: single shared FIFO queue
  intrusive_deque.hpp    Phase 1: hand-rolled per-worker doubly-linked deque
  work_stealing_pool.hpp Phase 1: per-worker deques + steal + overflow
src/
  global_queue_pool.cpp
  work_stealing_pool.cpp
tests/
  test_global_queue_pool.cpp
  test_intrusive_deque.cpp
  test_work_stealing_pool.cpp
bench/  benchmark.cpp     throughput micro-benchmark (flat workload)
eval/   evaluate.cpp      evaluation harness: throughput + p50/p99/p99.9
scripts/ run_sanitizers.sh, run_eval.sh
```

### Pool API

```cpp
class ThreadPool {
  virtual void enqueue(Task task) = 0;   // any thread, incl. inside a task
  virtual void wait() = 0;               // block until all submitted work done
  virtual std::size_t worker_count() const = 0;
  virtual const char* name() const = 0;
};
```

Both pools implement this interface so tests and the eval harness drive them
identically and the Phase 0→1 speedup is measured apples-to-apples. Shutdown is
RAII: the destructor sets a stop flag, wakes workers, joins them, and frees any
undrained nodes.

---

## 2. Phase 0 — baseline (global FIFO queue)  ✅

A single intrusive linked-list FIFO (`head`/`tail`) guarded by one
`std::mutex`. N workers block on a `condition_variable` until work arrives or
shutdown is signalled.

* **Why it does not scale:** every `enqueue` and every dequeue serializes on one
  lock, and each `enqueue` issues a `notify_one`. With trivial tasks the lock is
  the whole runtime. This is the intended baseline.
* **Completion tracking:** an atomic `pending_` counter is incremented *before*
  a task is published and decremented *after* it finishes; `wait()` blocks on
  `done_cv_` until `pending_ == 0`. Incrementing before publishing prevents a
  worker from racing `pending_` to zero before the producer accounts for the
  task.

Measured baseline (4 workers, 1M trivial tasks, Release): see the Benchmarks
section / phase report. It is deliberately slow and exists to be beaten.

---

## 3. Phase 1 — work-stealing  ✅

### 3.1 Intrusive deque (`intrusive_deque.hpp`)

A hand-rolled doubly-linked list of `TaskNode`s. Each deque has `front_`,
`back_`, a `size_`, and its own `std::mutex`.

```
front_ <-> n <-> n <-> n <-> back_
  ^steal end (thief)            ^owner end (push/pop)
```

* **Owner** operations work the *back*: `push_back` (submit), `pop_back` (take
  own most-recent work — LIFO, cache-hot).
* **Thief** operations work the *front*: `steal_front` takes the oldest task.
* All four operations take the deque's mutex. Phase 1 uses **fine-grained
  locking** — one lock *per deque* instead of one global lock — so contention is
  spread across N independent locks and the owner almost always finds the lock
  uncontended. (A fully lock-free Chase–Lev variant is a Phase 2 candidate; see
  §4. Doing it now would risk the "no data races" gate for marginal benefit on
  the common path, where the owner lock is already uncontended.)

**Deque invariants (asserted in code):**
1. `front_ == nullptr  ⇔  back_ == nullptr  ⇔  size_ == 0`.
2. Links are consistent: for any interior node `n`, `n->prev->next == n` and
   `n->next->prev == n`; `front_->prev == nullptr`, `back_->next == nullptr`.
3. A node is owned by exactly one deque (or one thread executing it). It is
   never linked into two queues; thus never run twice and never dropped.
4. `prev`/`next` are only read or written while the deque mutex is held.

### 3.2 Steal algorithm

```
worker_loop(self):
  loop:
    node = self.deque.pop_back()          # 1. own work first (LIFO, hot)
    if node: run(node); continue
    node = try_steal(self)                # 2. steal from a random victim
    if node: run(node); continue
    node = overflow.pop()                 # 3. global fallback
    if node: run(node); continue
    idle_wait()                           # 4. sleep until work or shutdown

try_steal(self):
  start = random_index()                  # randomized victim to avoid convoys
  for k in 0 .. N-1:
    victim = (start + k) % N
    if victim == self: continue
    node = deques[victim].steal_front()   # take oldest from the other end
    if node: return node
  return null
```

Random start spreads thieves across victims (avoids everyone hammering worker
0). Stealing from the *front* while the owner works the *back* means thief and
owner only contend when the deque has 0–1 elements.

### 3.3 Submission policy

* **From a worker thread** (recursive submit): push onto the *caller's own*
  deque back — cheap, keeps spawned work local and cache-hot.
* **From an external thread:** round-robin across worker deques (atomic
  counter). This spreads submission load over N locks instead of one, which is
  the key win over Phase 0 even for flat workloads.
* **Overflow:** each deque has a soft capacity cap; on overflow the task goes to
  the shared overflow queue, which every idle worker also drains. Guarantees no
  drops and bounds per-deque memory.

### 3.4 Idle / wakeup protocol & memory ordering

A worker that finds no work first **spins with exponential backoff** (a growing
run of `pause` instructions, escalating to `yield()`) as long as `pending_ > 0`
— work clearly exists somewhere and is cheap to grab once it surfaces; this is
what keeps a deep recursive computation saturating all workers. The backoff
keeps failed steals (each of which locks victim deques) from becoming a thief
storm that starves the threads doing real work.

When `pending_ == 0` there is genuinely nothing to do, so the worker **sleeps**
on a shared `condition_variable` instead of burning a core.

Avoiding lost wakeups without taking a global lock on every `enqueue` uses a
**Dekker-style handshake** on two `seq_cst` atomics:

* Producer (`enqueue` → `wake_one_worker`): `pending_.fetch_add(seq_cst)`, then
  `idle_workers_.load(seq_cst)`. If it sees zero sleepers it skips the wakeup
  entirely (the hot path under load).
* Worker (about to sleep): `idle_workers_.fetch_add(seq_cst)`, then re-check
  `pending_.load(seq_cst)`.

By sequential consistency at least one side observes the other: either the
producer sees `idle_workers_ > 0` and does the `lock(idle_mtx_)` + `notify`
dance, or the about-to-sleep worker sees `pending_ > 0` and does not sleep. So
no wakeup is lost, yet the common case costs only two atomics. On x86 the
`fetch_add`s are already `lock`-prefixed RMWs, so `seq_cst` is free here.

`wait()` uses the same `pending_` counter (incremented before a task is
published, decremented `acq_rel` after it completes) and a separate completion
CV; the worker that drives `pending_` to zero takes `done_mtx_` before notifying
so that wakeup cannot be lost either.

---

## 4. Phase 2 — refinements (in progress)

Phase 2 optimizes the hot path and finesses the design, strictly on top of green
Phase 1 tests. The chosen strategy is **staged, locks-first**: land the
refinements that are both easy to reason about and verifiable on the dev machine,
then add a lock-free deque as a clearly-flagged, *selectable* capstone (the
lock-based deque stays the default). The spec allows "lock-free **or**
fine-grained locking"; Phase 1's per-deque locks already make the owner path
uncontended, so the lock-based refinements below are a complete, valid Phase 2 on
their own.

### 4.1 Evaluation matrix (done)
The eval harness covers uniform / contended / producer-consumer / **sustained** /
**bursty** / **skewed** (uneven — a few fat tasks) loads, reporting throughput
and p50/p99/p99.9 scheduling latency, so each refinement is measured against the
load shapes the acceptance criteria require.

### 4.2 Graceful shutdown (planned)
An explicit, idempotent `shutdown(mode)`: `Drain` finishes all queued + in-flight
work then joins; `Cancel` lets in-flight tasks finish but discards the queued
backlog (freeing its nodes) and joins promptly. The destructor delegates to
`shutdown(Drain)`. `enqueue` after shutdown is ignored (never queued, never
counted). RAII and the join-before-free invariant are preserved.

### 4.3 Adaptive backoff (planned)
Refine the idle worker loop into a bounded spin (`cpu_relax`) → `yield` → CV
sleep ladder driven by consecutive empty rounds, with exposed steal-attempt /
steal-success / sleep counters for observability. Goal: keep deep recursive
computations saturating without a failed-steal thief storm.

### 4.4 Overflow throttling (planned)
Bound the overflow queue and back the producer off briefly when it is saturated,
so a burst of external submissions cannot grow memory without limit and idle
workers get a chance to drain it.

### 4.5 Batch stealing (planned)
`steal_half()` moves up to half the victim's nodes to the thief under a single
lock acquisition, amortizing the synchronization cost; the thief runs one and
pushes the rest onto its own deque. This is the main throughput lever for the
heavy-external-production patterns.

### 4.6 Lock-free Chase–Lev deque — capstone (planned, selectable)
A textbook Chase–Lev deque (atomic `top`/`bottom` over a growable circular array)
behind a selector, default off. Every atomic carries an inline memory-ordering
justification; the single-element pop-vs-steal race is resolved by the standard
CAS-on-`top`. Because TSan runs on Linux (not the Windows dev box), it ships with
an exact-count race-stress test that catches lost/duplicated nodes functionally,
and is validated under TSan before `v2` is tagged.

### 4.7 Memory-ordering strategy
`relaxed` for the producer side of `pending_` where a mutex already orders it;
`acq_rel`/`acquire` to publish/observe completion; the Chase–Lev path uses
`acquire`/`release` pairs with a `seq_cst` fence only where steal and pop race on
the last element. Every non-`seq_cst` atomic gets an inline justification.

## 5. Phase 3 candidates (scoped, not implemented)

google-benchmark integration; workload matrix (uniform / bursty / skewed /
producer-consumer / fib-DAG); scaling curves 1..N cores; flamegraph-driven
contention report; Dockerfile for reproducible runs.

---

## 6. Concurrency invariants (whole project)

1. A task is **in exactly one place** at any instant: one queue, or being run,
   or done. Never duplicated, never dropped. (Enforced by single-ownership of
   `TaskNode` and lock-protected splices; verified by tests counting exact runs
   and detecting double-runs.)
2. Only the **owner** mutates the back; only a **lock holder** mutates the
   front. (Phase 1: all mutation is under the deque lock.)
3. `pending_` is incremented before a task is visible and decremented after it
   completes ⇒ `wait()` never returns early.
4. All resources are RAII-managed; destructor joins all workers before freeing.

## 7. Test strategy & sanitizer gates

* Unit tests for the intrusive deque in isolation (link integrity, LIFO/FIFO
  ends, empty/one-element edge cases).
* Pool correctness: exact run-once counts, large sums, recursive submission,
  reusable `wait()`, FIFO ordering with a single worker.
* Load/balancing: skewed submission (all to one worker) must still finish via
  stealing; heavy fan-out trees.
* **No sleep-as-synchronization** in tests; use counters/CVs.
* Gates: **TSan** race-free and **ASan+UBSan** (with leak detection) clean are
  required before any phase is committed/tagged. Every prior phase's tests stay
  green (regression gate).

## 8. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Lost wakeup → hang | sleep only when `pending_==0`; producer notifies; `wait()` keyed on the same counter |
| Double-run / drop on steal vs pop of last element | both ends under the deque lock in Phase 1; deque unit tests cover the 1-element race |
| Memory leak of undrained tasks on shutdown | destructor drains and frees the lists |
| Benchmark noise | Release build, multiple runs, report median + percentiles |

## 9. Per-phase acceptance checklist

- [x] **Phase 0:** builds `-Wall -Wextra -Wpedantic -Werror`; 1M tasks complete
  correctly; TSan clean; ASan/UBSan clean; baseline throughput recorded.
- [x] **Phase 1:** intrusive deque with asserted invariants; steal logic;
  overflow fallback; throughput beats Phase 0 (multiplier recorded); skewed-load
  test passes via stealing; no task lost/duplicated; all Phase 0 tests still
  green; TSan + ASan/UBSan clean.
- [ ] Phase 2 / Phase 3: see §4–§5 (handed off).

## 10. Benchmarks (recorded)

See [`docs/PHASE_REPORTS.md`](docs/PHASE_REPORTS.md) for per-phase numbers and
the throughput multiplier from the evaluation harness.
