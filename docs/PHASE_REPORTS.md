# Phase Reports

Short report per phase: what was built, test results, benchmark numbers, key
design decisions, and any deviations from the plan.

---

## Phase 0 — Scaffold + baseline global-queue pool

**Built**
* CMake project (out-of-source, `-Wall -Wextra -Wpedantic -Werror`), directory
  layout, `.gitignore`, README + DESIGN.
* `Task` = `std::function<void()>`; intrusive `TaskNode` (reused by every queue).
* `ThreadPool` interface (`enqueue` / `wait` / `worker_count` / `name`).
* `GlobalQueuePool`: single shared FIFO (intrusive linked list) + one mutex + a
  condition variable; N workers; atomic `pending_` completion tracking; RAII
  shutdown (stop flag → notify → join → drain).
* GoogleTest suite (6 tests) + custom throughput benchmark.

**Tests** — all green:
`RunsASingleTask`, `AllTasksRunExactlyOnce` (100k), `SumIsCorrectUnderConcurrency`
(200k), `RecursiveSubmissionFromInsideTasks`, `WaitIsReusableAcrossMultipleBatches`,
`FifoOrderingFromSingleProducerSingleWorker`.

**Sanitizer gates** — clean:
* ThreadSanitizer: 0 data races.
* AddressSanitizer + UBSan (leak detection on): 0 issues.

**Benchmark** (4 workers, 1,000,000 trivial tasks, Release `-O3`):

| Pool | Workers | Throughput |
|------|--------:|-----------:|
| global-queue | 4 | ~0.05 Mtasks/s |

The single global lock plus a `notify_one` per enqueue serializes everything —
this is the intended, deliberately-slow baseline for Phase 1 to beat.

**Design decisions**
* Reuse the intrusive `TaskNode` as the FIFO storage so Phase 0 and Phase 1
  share allocation behaviour and the comparison is fair.
* Increment `pending_` *before* publishing a task so `wait()` cannot observe a
  premature zero.

**Deviations** — GoogleTest is fetched as a release **tarball over HTTPS**
instead of a `git clone`, because the sandbox's git proxy is scoped to this one
repository. Functionally identical; content is pinned by SHA-256.

---

## Phase 1 — Work-stealing (per-worker intrusive deques + steal logic)

**Built**
* `IntrusiveDeque`: hand-rolled doubly-linked deque of `TaskNode` with a
  per-deque mutex. Owner uses the back (LIFO: `try_push_back` / `pop_back`),
  thieves use the front (`steal_front`); soft capacity → overflow. Structural
  invariants asserted, plus a `validate()` used by tests.
* `WorkStealingPool`: one deque per worker; idle workers steal from a
  **randomly chosen** victim's front; single **overflow queue** fallback so no
  task is dropped. Recursive submissions go to the caller's own deque (cache
  hot); external submissions round-robin across deques to spread lock load.
* Idle protocol: spin-with-backoff while work exists, sleep on a CV when truly
  idle, lost-wakeup-free via a `seq_cst` Dekker handshake (see DESIGN §3.4).
* 16 new tests (7 deque + 9 pool), incl. skewed-load balancing, a fib task DAG,
  many external producers, and a concurrent owner-vs-thieves deque stress test.

**Tests** — all 22 green (6 Phase 0 + 7 deque + 9 work-stealing); the Phase 0
suite stays green (regression gate held).

**Sanitizer gates** — clean: ThreadSanitizer (0 races) and ASan+UBSan (0 issues,
leak detection on), full suite.

**Benchmark / evaluation** (4 workers; eval harness = warmup + median of 3 runs).
Throughput speedup over Phase 0, and task scheduling latency p50/p99/p99.9 (µs,
enqueue → start of execution):

| Workload | global Mtask/s | WS Mtask/s | speedup | global p50 | WS p50 |
|----------|---------------:|-----------:|--------:|-----------:|-------:|
| recursive fan-out 2²⁰ | ~1.2 | ~2.5 | **~2.1x** | — | — |
| contended (4 prod, trivial) | ~0.86 | ~1.72 | **~2.0x** | 9187 µs | **370 µs** |
| prod/cons (4 prod, ~300 ns) | ~1.02 | ~1.43 | **~1.4x** | 17784 µs | **452 µs** |
| bursty (8 prod, ~150 ns) | ~1.09 | ~1.43 | **~1.3x** | 99040 µs | **1962 µs** |
| uniform (1 prod, trivial) | ~0.05 | ~0.06 | ~1.1x | 13 µs | 12 µs |

The latency win is the headline: distributing work across N deques collapses
queueing delay (p50 from milliseconds to hundreds of µs). Throughput improves
1.3–2.1x on every parallel/recursive pattern.

**Design decisions**
* Phase 1 uses **fine-grained locking** (one mutex per deque), not a lock-free
  deque. The owner lock is almost always uncontended, thieves spread across N
  locks, and it keeps the "zero data races" gate trivially satisfiable. A
  lock-free Chase–Lev deque is a Phase 2 candidate (DESIGN §4).
* Randomized victim selection avoids steal convoys.
* `seq_cst` Dekker handshake lets `enqueue` skip the wakeup machinery whenever no
  worker is asleep — the common case under load — with no lost wakeups.

**Known limitation** — single-producer trivial tasks are producer-bound (≈parity
with the baseline): the lone enqueuing thread is the serial bottleneck, which no
scheduler can parallelize. Work-stealing's wins appear when work is generated in
parallel or recursively. Benchmark numbers also vary on a shared machine; the
recursive result is the most stable. Phase 2 (batch stealing, adaptive backoff)
targets the heavy-external-production case further.

**Deviations** — none beyond the Phase 0 GoogleTest fetch note.
