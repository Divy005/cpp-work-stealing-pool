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
