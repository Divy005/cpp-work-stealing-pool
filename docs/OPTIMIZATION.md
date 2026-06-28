# Optimization & Contention Analysis

This document explains how to use the built-in observability counters to diagnose
pool behaviour, interpret what each metric means under different workloads, and
describes the perf/flamegraph methodology for deeper profiling.

---

## 1. Observability counters

`WorkStealingPool` exposes six monotonic counters (relaxed atomics, read
contention-free, never gate correctness):

| Counter | Method | What it counts |
|---------|--------|----------------|
| `steals_` | `steals()` | Successful steal *operations* (each moves ≥ 1 task) |
| `stolen_tasks_` | `stolen_tasks()` | Total tasks moved across all steals |
| `steal_attempts_` | `steal_attempts()` | Total steal sweeps attempted |
| `sleeps_` | `sleeps()` | Times a worker parked on the idle CV |
| `overflow_pushes_` | `overflow_pushes()` | Tasks that landed in the overflow queue |
| `overflow_throttles_` | `overflow_throttles()` | Times an external producer backed off |

The evaluation harness (`wsp_eval`) prints these after a work-stealing run. The
benchmark (`wsp_bench`) also prints a one-line summary.

### Derived metrics

```
steal hit rate   = steals / steal_attempts      (fraction of sweeps that found work)
avg batch size   = stolen_tasks / steals         (tasks moved per successful steal)
sleep fraction   = sleeps / (tasks + steals)     (proxy for idle time)
overflow rate    = overflow_pushes / total_tasks (fraction landing in overflow)
```

---

## 2. Interpreting counter profiles

### 2.1 Healthy recursive / divide-and-conquer workload

```
steals=4312  stolen_tasks=18540  steal_attempts=9201  sleeps=0  overflow=0
steal hit rate = 46.9%   avg batch = 4.3   sleep fraction ≈ 0
```

* High hit rate and batch > 1: the deques have deep sub-trees that pay off per
  steal. Workers stay busy with almost zero sleep.
* `overflow = 0`: workers consume tasks as fast as they arrive; deques never fill.
* This is the target profile — work-stealing operating exactly as intended.

### 2.2 Single-producer trivial tasks (producer-bound)

```
steals=0  stolen_tasks=0  steal_attempts=12450  sleeps=8200  overflow=0
steal hit rate = 0%   sleep fraction = high
```

* Workers constantly find empty deques: the single producer is the bottleneck.
  Stealing is futile because there is nothing to steal.
* High sleep count: workers park quickly since there is no pending work between
  producer deliveries.
* Work-stealing provides no benefit here; throughput matches the global-queue
  baseline.

### 2.3 Overloaded external producers (overflow visible)

```
steals=1200  stolen_tasks=2400  steal_attempts=3000  sleeps=100
overflow_pushes=18000  overflow_throttles=320
```

* High `overflow_pushes`: producers are submitting faster than workers drain the
  deques; tasks spill to the fallback queue.
* `overflow_throttles > 0`: the soft cap was hit; external producers backed off for
  up to `kMaxThrottleRounds` spin rounds. If throttles are very high, consider
  increasing workers or reducing submission rate.
* Tasks still complete — nothing is dropped. The overflow queue guarantees fairness.

### 2.4 Skewed (fat-task) workload

```
steals=850  stolen_tasks=6200  steal_attempts=1900  sleeps=400
steal hit rate = 44.7%   avg batch = 7.3
```

* The few fat tasks (16× work) dominate. Idle workers steal large sub-trees from
  the busy worker's deque — avg batch > 1 confirms the batch-stealing amortization.
* Moderate sleeps: the pool is never truly overloaded but workers do park briefly
  between fat-task completions.

---

## 3. Tuning knobs

| Knob | Where | Default | Effect |
|------|-------|---------|--------|
| `deque_capacity` | `WorkStealingPool` ctor | 4096 | Larger → fewer overflow spills; more memory per worker |
| `kMaxSpinShift` | `work_stealing_pool.cpp` | 10 (1024 pauses) | Higher → more aggressive spinning before yielding |
| `kYieldAfter` | `work_stealing_pool.cpp` | 20 rounds | Lower → yield sooner, friendlier to OS scheduler |
| `kMaxThrottleRounds` | `work_stealing_pool.cpp` | 64 | Higher → producers back off longer; more smoothing |
| Worker count | `WorkStealingPool` ctor | `hardware_concurrency()` | Tune to avoid hyperthreading noise at high counts |

Changing `kMaxSpinShift` has the largest impact on recursive workloads: a higher
value keeps workers hot during deep task trees but burns CPU during idle gaps.

---

## 4. Flamegraph / perf methodology (Linux)

Use `perf record` + Brendan Gregg's flamegraph scripts to find the hottest
functions. The Docker image provides the required Linux environment (see
[`Dockerfile`](../Dockerfile)).

### 4.1 Capture

```bash
# Inside the container:
perf record -F 997 -g --call-graph dwarf -- /src/build/wsp_bench
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

Frequency 997 Hz avoids aliasing with 1 kHz timer intervals. `--call-graph dwarf`
gives accurate frames through inlined STL wrappers.

### 4.2 What to look for

| Hot frame | Diagnosis |
|-----------|-----------|
| `std::mutex::lock` inside `IntrusiveDeque` | Thief–owner contention; increase deque capacity or use fewer workers |
| `enqueue → push_overflow` | Submission rate exceeds drain rate; add workers |
| `idle_cv_.wait` (worker sleeping) | Workers not finding work — more tasks per deque needed |
| `cpu_relax` / `pause` | Workers spinning during backoff; tuning `kMaxSpinShift` may help |
| `std::function::operator()` overhead | Tasks are too trivial; switch to a task type with less indirection |

### 4.3 Key ratios from perf stat

```bash
perf stat -e cache-misses,cache-references,context-switches,migrations \
    /src/build/wsp_bench
```

* **cache-miss rate > 5%** on the worker loop: deque nodes are not cache-hot;
  consider LIFO order (already the default) or task object packing.
* **context-switches > sleep count**: the OS is scheduling workers out — consider
  `SCHED_FIFO` or reducing worker count to physical cores.
* **cpu-migrations**: moving workers between NUMA nodes; pin with `taskset` or
  `numactl`.

---

## 5. Known hot paths and their mitigations

### 5.1 Per-deque mutex — the owner fast path

The owner (`try_push_back` / `pop_back`) almost always acquires the mutex
uncontended on the recursive path: the thief only holds it briefly for a steal,
and the owner is the only writer of its back. On x86 an uncontended `lock cmpxchg`
(the mutex's compare-exchange) costs ~10 cycles — indistinguishable from background
noise for tasks that do more than a handful of instructions.

Contention rises when:
* Tasks are trivially short and deques are small (thief and owner both hammer the
  back frequently).
* Worker count > physical cores (hyperthreads share a core, doubling lock traffic
  on shared deques).

The Chase–Lev deque (DESIGN §4.6) eliminates the owner-side lock entirely.

### 5.2 Overflow queue contention

The overflow queue has its own mutex, independent of any deque lock. Under normal
load it is drained rapidly and is rarely accessed. Contention only occurs during a
submission burst; the throttle mechanism reduces it by slowing producers.

### 5.3 Lost-wakeup-free Dekker handshake cost

The producer side of the handshake (`pending_.fetch_add(seq_cst)`) is a locked
RMW on x86 — effectively free relative to the task dispatch work. The worker's
seq_cst load before sleeping is also a memory barrier that costs ~20 cycles but
executes at most once per sleep event (rare under load).

---

## 6. Summary: counter reading cheat sheet

```
High steal hit rate + batch > 1      → healthy; work-stealing paying off
Zero steals + high sleep             → producer-bound; steal can't help
overflow_pushes > 0                  → producers outpacing workers; add workers
overflow_throttles >> 0              → severe burst; increase kMaxThrottleRounds or deque_capacity
low sleeps + low steals              → good balance; most work consumed by owner
sleeps >> tasks                      → trivial tasks; the pool overhead dominates
```
