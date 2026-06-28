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

---

## Phase 2 — Refinements (shutdown, backoff, overflow throttling, batch stealing, lock-free capstone)

Strategy: **staged, locks-first** (DESIGN §4). The spec permits lock-free *or*
fine-grained locking, so the lock-based deque plus batch stealing is the
production path and the lock-free Chase–Lev deque is a documented, standalone
demonstration — keeping the default easy to follow and locally verifiable.

**Built**
* **Graceful shutdown** — `ShutdownMode{Drain,Cancel}` + idempotent `shutdown()`
  on the interface and both pools. Drain finishes queued + in-flight work then
  joins; Cancel finishes in-flight work, discards the backlog (freeing nodes and
  reconciling `pending_` so a stray `wait()` can't hang), then joins promptly.
  The destructor delegates to `shutdown(Drain)`; `enqueue` no-ops after shutdown.
* **Adaptive backoff + observability** — the idle ladder (bounded exponential
  spin → yield → CV sleep) now uses named tiers; new `steal_attempts()`,
  `stolen_tasks()` and `sleeps()` counters expose the steal hit rate and parking
  (printed by the benchmark).
* **Overflow throttling** — the overflow queue has a soft cap (~one
  deque-capacity per worker); an external producer backs off (bounded) when it
  saturates, bounding memory under a submission storm. Worker submits are never
  throttled and nothing is dropped.
* **Batch stealing** — `IntrusiveDeque::steal_half()` moves up to half a victim's
  deque under one lock; the thief runs the oldest task and stashes the rest on
  its own deque, amortizing the victim-side steal lock over a run of work.
* **Lock-free capstone** — `ChaseLevDeque`, a textbook fixed-capacity Chase–Lev
  deque (atomic top/bottom, CAS steal, seq_cst CAS for the single-element tie),
  every atomic justified inline. Standalone; does not replace the default deque.

**Tests** — all **40 green** (was 22): +7 shutdown, +1 overflow throttling,
+3 `steal_half`, +7 Chase–Lev (including a 200k-node × 3-thief exact-count
race-stress, clean over 25× repeat), plus observability assertions. Every
Phase 0/1 test stays green (regression gate held).

**Sanitizer gates** — Phase 2 was developed on a Windows / MSYS2 g++ 13.1.0 box
where TSan is unavailable and ASan unreliable; per the agreed workflow the
**TSan + ASan/UBSan gates run on Linux/CI before `v2` is tagged**. Functionally,
the lock-free deque's exact-count race-stress test catches any lost/duplicated
node.

**Evaluation** (Windows dev box, 20 workers, gcc 13.1.0 `-O2`; eval harness =
warmup + median of 3 runs, 200k tasks). Speedup over the Phase 0 baseline and
work-stealing scheduling latency p50/p99/p99.9 (µs):

| Workload | global Mtask/s | WS Mtask/s | speedup | WS p50 | WS p99 | WS p99.9 |
|----------|---------------:|-----------:|--------:|-------:|-------:|---------:|
| uniform (1 prod, trivial) | 0.039 | 0.038 | 0.96x | 8.5 | 73.7 | 108.7 |
| contended (4 prod, trivial) | 0.045 | 0.099 | **2.18x** | 7.6 | 428.9 | 1065.8 |
| prod/cons (4 prod, ~300ns) | 0.047 | 0.116 | **2.44x** | 6.5 | 266.6 | 890.3 |
| sustained (2 prod, ~600ns) | 0.034 | 0.040 | 1.16x | 8.8 | 85.0 | 127.9 |
| bursty (8 prod, ~150ns) | 0.108 | 0.782 | **7.25x** | 8.4 | 780.8 | 1849.1 |
| skewed (4 prod, 1/16 fat) | 0.044 | 0.103 | **2.35x** | 6.3 | 157.3 | 529.1 |

Work-stealing holds p50 scheduling latency at 6–9 µs across every pattern (vs
12–18 µs for the global queue) and improves throughput on every parallel pattern
(up to 7.25x bursty). Single-producer uniform is producer-bound (≈parity), as in
Phase 1. These are 20-core Windows figures and differ from the 4-worker Linux
numbers in the Phase 1 report; they are re-measured on Linux as part of the
sanitizer gate.

**Design decisions** — fixed-capacity Chase–Lev to avoid the growable-array
reclamation hazard; batch stealing redistributes onto the thief's own
(uncontended) deque; Cancel reconciles `pending_`; observability counters are
relaxed atomics that never gate correctness.

**Deviations** — Phase 2 was developed on Windows (MSYS2 g++), so the TSan/ASan
gate runs on Linux before tagging rather than locally. This was agreed up front
and is the only divergence from the per-phase workflow.
