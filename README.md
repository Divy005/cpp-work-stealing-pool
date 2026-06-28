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
| **1** | Work-stealing: per-worker intrusive deques + steal logic | ✅ done (`v1`) |
| **2** | Graceful shutdown, adaptive backoff, overflow throttling, batch stealing, lock-free deque | ✅ done (`v2`) |
| **3** | Benchmark suite, demo workloads, observability docs, Dockerfile | ✅ done (`v3`) |

All four phases are complete. Phase 3 adds:

* **Demo workloads** — fib task DAG and parallel sort, correctness-verified (`demo/demo.cpp`).
* **Benchmark report** — throughput+latency matrix, scaling curve, micro-bench ([`BENCHMARKS.md`](BENCHMARKS.md)).
* **Contention analysis** — counter profiles, tuning knobs, perf/flamegraph guide ([`docs/OPTIMIZATION.md`](docs/OPTIMIZATION.md)).
* **Memory-ordering audit** — inline justification on every non-`seq_cst` atomic.
* **Dockerfile** — Ubuntu 24.04 image for reproducible Linux builds and TSan/ASan/UBSan gates.

Sanitizer note: the TSan + ASan/UBSan gates run inside the Docker container
(Linux environment required for TSan); the Dockerfile provides the gate.

## Results (median of 3 runs, 4 workers; see `docs/PHASE_REPORTS.md`)

Work-stealing throughput speedup over the Phase 0 baseline, and task scheduling
latency (enqueue → start of execution):

| Workload | speedup | global p50 | work-stealing p50 |
|----------|--------:|-----------:|------------------:|
| recursive fan-out (2²⁰ tasks) | ~2.1x | — | — |
| contended (4 producers, trivial) | ~2.0x | 9187 µs | **370 µs** |
| producer/consumer (4 prod, ~300 ns) | ~1.4x | 17784 µs | **452 µs** |
| bursty (8 producers, ~150 ns) | ~1.3x | 99040 µs | **1962 µs** |

Numbers vary on a shared machine; the recursive win is the most stable.
Single-producer trivial tasks are producer-bound (≈parity) — work-stealing helps
when work is generated in parallel or recursively, which is its design purpose.

## Architecture (current)

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
demo/          divide-and-conquer demo (fib DAG + parallel sort)
scripts/       sanitizer + eval runner scripts
docs/          design notes / phase reports / optimization guide
Dockerfile     reproducible Linux build + TSan/ASan/UBSan gates
```

## Further reading

| Document | Contents |
|----------|----------|
| [`DESIGN.md`](DESIGN.md) | Architecture, data structures, steal algorithm, synchronization invariants, memory-ordering rationale, per-phase acceptance criteria |
| [`BENCHMARKS.md`](BENCHMARKS.md) | Throughput + latency matrix, scaling curve, micro-benchmark, demo numbers, reproduction instructions |
| [`docs/OPTIMIZATION.md`](docs/OPTIMIZATION.md) | Observability counter guide, contention analysis, tuning knobs, perf/flamegraph methodology |
| [`docs/PHASE_REPORTS.md`](docs/PHASE_REPORTS.md) | Per-phase build log, test results, benchmark numbers, design decisions |
