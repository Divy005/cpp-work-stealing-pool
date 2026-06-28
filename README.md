# cpp-work-stealing-pool

A high-performance, **work-stealing thread pool scheduler** written in modern
C++17, built from first principles. No off-the-shelf thread pools, queues, or
schedulers — the synchronization primitives, the per-worker deques, the steal
logic, and the scheduling are all hand-written. This is a systems/concurrency
artifact: the point is correct *and* fast concurrency infrastructure.

## Status

| Phase | Description | State |
|-------|-------------|-------|
| **0** | Scaffold + baseline thread pool (single global FIFO queue) | ✅ done (`v0`) |
| 1 | Work-stealing: per-worker intrusive deques + steal logic | in progress |
| 2 | Lock-free / batch-steal / backoff refinements | planned |
| 3 | Benchmark suite, observability, full design docs | planned |

Phases 1–3 are scoped in [`DESIGN.md`](DESIGN.md).

## Architecture (target end state)

```
 caller (any thread) ── enqueue(task) ─┐
                                       │ external tasks: round-robin into deques
   ┌───────────────────────────────────┼───────────────────────────────┐
   ▼                ▼                   ▼                                ▼
┌────────┐     ┌────────┐         ┌────────┐                      ┌──────────────┐
│Worker 0│     │Worker 1│   ...   │Worker N│                      │ overflow MPMC │
│ deque  │     │ deque  │         │ deque  │  <── steal (front)── │ queue (FIFO)  │
└───┬────┘     └───┬────┘         └───┬────┘                      └──────────────┘
 push/pop back  push/pop back     push/pop back        fallback when a deque is full
 (owner, LIFO)                    steal from front (thief, FIFO)
```

* Each worker owns a **LIFO deque**: it pushes/pops at the *back*. This keeps
  recently-created (cache-hot) work local and the common path uncontended.
* An idle worker **steals from the *front*** of a randomly chosen victim — the
  opposite end from the owner, which minimizes head-to-head contention and
  tends to steal the oldest, largest sub-trees of work.
* A single **overflow queue** is the fallback so no task is ever dropped and
  every task eventually runs (fairness).

See [`DESIGN.md`](DESIGN.md) for the deque internals, the steal algorithm, the
synchronization invariants, and the memory-ordering rationale.

## Building

Requires CMake ≥ 3.16, a C++17 compiler, and pthreads. GoogleTest is fetched
automatically (release tarball over HTTPS — no system install needed).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Benchmark

```bash
./build/wsp_bench
```

### Evaluation / inference harness

A standalone harness runs several workload patterns against both pools and
reports throughput plus latency percentiles (p50/p99/p99.9), so results are
easy to evaluate and reproduce:

```bash
./build/wsp_eval                 # default settings
./build/wsp_eval --workers 8 --tasks 2000000
./scripts/run_eval.sh            # pinned, multi-run summary
```

### Sanitizer gates (required, must be clean)

```bash
# ThreadSanitizer — zero data races
cmake -S . -B build-tsan -DWSP_SANITIZER=thread -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j && ./build-tsan/wsp_tests

# AddressSanitizer + UBSan — zero memory/UB bugs, zero leaks
cmake -S . -B build-asan -DWSP_SANITIZER=address,undefined -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j && ./build-asan/wsp_tests
```

Or run both gates via:

```bash
./scripts/run_sanitizers.sh
```

## Layout

```
include/wsp/   public headers (task, interface, pools, intrusive deque)
src/           pool implementations
tests/         GoogleTest correctness + load tests
bench/         throughput micro-benchmark
eval/          evaluation/inference harness (throughput + latency percentiles)
scripts/       sanitizer + eval runner scripts
docs/          design notes / phase reports
```
