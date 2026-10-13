# Tail Latency Lab — C++20

A small, dependency-free experiment for the companion article **Fast Most of the Time: Investigating Tail Latency in Modern C++**.

This is an order-state processing kernel, not an exchange simulator, complete order book, or production HFT benchmark. It isolates mechanisms and preserves a deterministic business-work trace across variants. The article includes 90 local one-million-event trials on an Apple M4 Mac mini: these short desktop measurements are not evidence of production performance. See [measured results](results/RESULTS.md).

## Build and verify

Requirements: a C++20 compiler and standard library, Make (optional), Python 3 for tests/suite. Linux is needed only for the suggested perf/taskset experiments.

```bash
make
make test
# Without Make:
g++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic src/benchmark.cpp -o tail_bench
python3 tests/test_benchmark.py
```

No third-party C++ dependencies. Compile sanitizer builds separately from performance measurements. Tests check lifetime/state behavior, equivalent checksums, timing identities, growth/pause markers, output counts, log size, quantiles and invalid inputs. They do not assert machine-dependent speedups.

## First experiments

```bash
# Pilot: maximum measured replay throughput, no offered-load waiting metric.
./tail_bench --mode baseline --events 1000000 --interval-ns 0 --output results/pilot
# Illustrative pacing value, NOT a recommended universal production rate:
./tail_bench --mode baseline --events 1000000 --interval-ns 1000 --output results/base
python3 run_suite.py --events 1000000 --interval-ns 1000 --repeats 5
```

Use the baseline pilot throughput R to choose common offered rates of approximately 0.25R, 0.50R and 0.80R. For target rate r events/s, use interval_ns = round(1e9/r). Reuse the same interval across all modes in a comparison. Do not normalize every mode to its own capacity. The instrumented pilot includes timestamp and sample-storage overhead; R is harness-specific.

## Six variants and exact comparisons

| Mode | Change relative to baseline | Interpretation |
|---|---|---|
| baseline | Fixed order slots; audit vector reserved for 4,096 entries; no file logging | Reference |
| allocate | make_unique on add; destruction on cancel/execute | Per-order allocation/lifetime versus reused slots |
| grow | Release audit storage every 4,096 events; allow regrowth | Repeated vector growth versus capacity reuse |
| log-buffered | Write one 8-byte binary checksum per event to a 64 KiB stdio buffer | Additional buffered logging work |
| log-flush | Same record and buffer; fflush after every event | Flush policy comparison with log-buffered |
| pause | Busy-wait at least 50 microseconds every 10,000 events | Synthetic measurement control, NOT scheduler evidence |

`fflush` is not `fsync`: these modes do not establish crash durability. Buffered logging's final flush is outside event latency and reported separately. The per-event record is deliberately minimal; no text formatting or async logger is implemented. Comparing no logging with logging changes the output requirement; it is a cost decomposition, not a semantics-preserving production optimization.

The grow variant is an intentionally stressed scratch-buffer lifecycle, not a claim that mature containers constantly reallocate. Capacity changes are explicitly marked. Library growth factors are implementation-dependent.

## Workload

4,096 add events followed by 4,096 removals: even slots execute fully, odd slots cancel. Repeat this deterministic cycle. In a full cycle the mix is 50% add, 25% cancel, 25% execute. Prices are positive integer ticks; there is no floating-point price arithmetic. Every variant uses the same trace and checksums. Incomplete final cycles are allowed and leave active orders, so report event counts. There is no price discovery, matching priority, network, parser, multi-instrument state, contention inside the engine, or partial fill model.

## Timing boundaries and replay model

Use `steady_clock`. With pacing enabled, intended arrival of event i is origin + i * interval. The loop waits until that time, or starts immediately if already behind. It does not shift later arrival targets when the kernel stalls. Record:

- service = end - start (wall time, including preemption within the interval);
- wait = start - intended arrival;
- response = end - intended arrival = wait + service.

This is a single-threaded scheduled replay with a **virtual backlog**, not a real producer/consumer queue. Wait includes harness lateness as well as backlog. The entire trace is pre-generated; replay is effectively lossless with no queue capacity limit. Instrumentation after the end timestamp contributes to the next event's wait. Do not label response network round-trip or exchange end-to-end latency.

With interval=0, pacing is disabled; wait is defined as zero and response equals service. It is only a throughput/service pilot. Fixed-rate scheduled replay avoids one common coordinated-omission trap, but does not reproduce bursty external traffic or a finite queue's backpressure/loss.

## Outputs

Each prefix creates `.samples.csv`, `.summary.csv`, and a `.bin` file for logging modes. Summary contains nearest-rank p50/p99/p99.9, observed max, count above an illustrative 100-microsecond threshold, measured events/s, checksum, capacity-growth count, pause count and final-flush time. All latency values are nanoseconds. The threshold is not an SLA.

Raw timestamps allow independent quantile recomputation and event-to-event analysis. Data is written to CSV after timing. Sample storage is allocated and initialized first; nevertheless timestamp calls, sample writes and loop logic perturb measurements. Large runs consume memory and disk: 30 runs of a million events can produce several GB of CSV. Retain raw data deliberately and exclude it from Git unless desired. The suite randomizes mode order with seed 42 and retains per-run summaries; it does not average percentiles.

The warm-up runs the order kernel for eight complete cycles. It does not warm all file I/O paths or the growth scenario. First-use behavior remains in measured variants; compare first-window and later-window data rather than silently discarding it.

## Scheduler investigation (separate from injected pause)

Choose a CPU allowed by your environment; CPU 2 below is only an example.

```bash
taskset -c 2 ./tail_bench --mode baseline --events 1000000 --interval-ns 1000 --output results/pinned
perf stat -e context-switches,cpu-migrations,page-faults,cycles,instructions,cache-misses -- ./tail_bench --mode baseline --events 1000000 --interval-ns 1000 --output results/counters
perf sched record -- ./tail_bench --mode baseline --events 1000000 --interval-ns 1000 --output results/scheduling
perf sched timehist
```

Permissions and event availability vary. Compare unpinned/pinned, idle/controlled CPU contention with the same trace and rate. Record the competing workload and its affinity. Pinning is not CPU isolation and does not remove interrupts/preemption. Whole-process `perf stat` includes setup and CSV export: use it as a coarse signal, not hot-path-only attribution. Scheduler traces need explicit alignment to benchmark intervals before attributing any particular outlier. Raw CSV offsets are relative to a local steady-clock origin, not automatically to perf's timestamp domain. Profiling runs should be separate from clean timing runs and their overhead disclosed.

## Publication protocol

Run at least five independent trials at each offered rate, target at least one million completed events per trial, and extend duration for periodic OS effects. Record CPU/model, topology, allowed affinity, RAM, kernel, compiler, standard library, allocator, build flags, governor/turbo, virtualization, filesystem/log destination, process load and commit hash. Report per-trial p99.9 and its range; do not average per-run quantiles into a claimed pooled percentile. If a pooled percentile is wanted, compute it from pooled raw observations and also show run variability.

Create a timeline near an outlier, a complementary CDF for service/response, and a per-trial p99.9 comparison. For attribution, require a matching event marker/trace plus a controlled paired change. Aggregate cache misses or context switches alone do not explain an individual outlier. No numeric speedup or production suitability is claimed by this repository.

## Repository contents

- `ARTICLE.md`: complete English methodology article and benchmark specification.
- `src/benchmark.cpp`: experiment and built-in state/quantile tests.
- `tests/test_benchmark.py`: separate integration tests.
- `run_suite.py`: repeated randomized trials and summary collection.
- `results/VALIDATION.md`: build and correctness checks from the local run.
- `results/RESULTS.md`: measured results, per-trial tails, environment and reproduction commands.

No license has been selected on your behalf; add your chosen license before inviting reuse under explicit licensing terms.
