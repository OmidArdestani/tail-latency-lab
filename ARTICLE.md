# Fast Most of the Time: Investigating Tail Latency in Modern C++

*A reproducible experiment with allocation, container growth, logging and scheduled order-event replay.*

**Status:** Methodology article with runnable C++20 experiments. The code has been checked for correctness in the authoring environment. No production HFT performance results or measured speedups are claimed. The empirical results section is explicitly pending controlled measurements on the target machine.

## 1. The request that the average hides

A processing path can look excellent in a throughput benchmark and still respond poorly to an occasional event. For a trading system, the important question is not only how much work it completes, but how long a particular event waits before its result becomes available.

This changes the optimization problem. A small improvement to the common path may matter less than understanding an infrequent allocation, buffer expansion, I/O operation or scheduling delay. It also changes how we measure: a fast function is not the same thing as a fast response when work is waiting ahead of it.

In this article, I use a deliberately small C++20 order-state kernel to investigate those mechanisms. The objective is not to declare one allocator, container or logging policy universally superior. It is to connect an observed delay to evidence, change one relevant behavior, and check whether the observation survives repeated measurement.

## 2. What p99 and p99.9 actually tell us

The 99th percentile is a boundary in an observed latency distribution. Approximately 99% of samples fall at or below it; p99.9 moves that boundary to approximately 99.9%. The exact sample selected depends on the quantile convention. The accompanying code uses nearest rank: sort N samples and select the element at one-based position ceil(p × N).

Neither percentile is a worst-case guarantee. A delay that occurs much less often than once per thousand events can be almost invisible in p99.9. An observed maximum is useful evidence, but it is not a bound on what the next run could experience.

For this reason, I report p50, p99, p99.9, maximum, sample count and a threshold-exceedance count. A timeline supplies the ordering information that a histogram loses. One million observations put roughly one thousand observations above the p99.9 boundary, but temporal dependence and periodic effects still matter: a large sample count alone does not establish statistical stability.

## 3. Define the latency before measuring it

I distinguish three timestamps for event i:

| Timestamp | Meaning |
|---|---|
| aᵢ | Intended arrival time in the replay schedule |
| sᵢ | Start of event processing |
| eᵢ | Completion of event processing |

The measured quantities are:

- **Service time:** eᵢ − sᵢ.
- **Waiting time:** sᵢ − aᵢ.
- **Response time:** eᵢ − aᵢ = waiting time + service time.

Service time here is elapsed wall time, not CPU execution time. If the OS deschedules the thread between start and completion, that delay belongs to the observed service interval.

A slow event can also affect its successors. Once the worker falls behind its arrival schedule, later events may have ordinary service times and elevated response times. Optimizing only the code between sᵢ and eᵢ can therefore miss the delay experienced by incoming work.

The companion harness models this with single-threaded scheduled replay and a virtual backlog. It does not implement a physical network or producer/consumer queue. Its waiting metric includes harness scheduling lateness; its response metric must not be called exchange round-trip latency.

## 4. Avoid a benchmark that waits for the system to recover

A problematic load generator sends an event, waits for completion, and only then decides when to send another. When the system stalls, that generator reduces the offered load. It can omit observations that an independently arriving stream would have created. This is the coordinated-omission problem discussed in the HdrHistogram project. [1]

The paced harness assigns arrival targets in advance:

`aᵢ = origin + i × interval`

A late completion does not move subsequent targets forward. The replay catches up as fast as it can, preserving the waiting time implied by the schedule. This addresses that particular omission mechanism; it does not make a periodic synthetic stream equivalent to real market traffic.

The experiment has no finite queue or drop policy. Under overload, virtual waiting can grow substantially. A real bounded system would also require measurements of rejection, loss, backpressure and recovery. Those are outside this first experiment.

## 5. Keep business work constant

The kernel maintains 4,096 reusable order slots. Each complete workload cycle first adds 4,096 orders, then removes them: half through cancellation and half through full execution. Thus a complete cycle is 50% add, 25% cancel and 25% execute. Prices use integer ticks.

Every variant replays the same deterministic events. A running checksum depends on removed order prices, operation types and slots. Tests verify equivalent checksums and reject invalid state transitions.

This is intentionally not a complete price-time matching engine. It does not model price-level search, partial fills, market-data decoding, network transport or concurrent ownership. Keeping the kernel small makes the experiment easier to inspect, at the cost of limiting what its results can establish about a production system.

## 6. Experiment A: Per-order allocation versus reused storage

The baseline uses fixed order storage. The allocation variant constructs an order with `std::make_unique` on add and destroys it on cancellation or execution. Both perform the same order-state transitions.

The question is whether changing the allocation/lifetime strategy changes the service-time distribution for this workload. The experiment does not assume that every allocation enters the kernel or is slow: an allocator may satisfy repeated requests cheaply from already available memory.

Interpret the comparison as a strategy-level change. Storage locality and memory reuse change along with allocation calls. A difference is not proof that allocator synchronization or a page fault was its sole cause. More specific attribution needs additional instrumentation and controlled experiments.

Modern C++ provides tools for explicit lifetime and ownership; it does not remove the need to choose a memory policy. A pool also introduces capacity management and reuse concerns. Faster measurements would not, by themselves, settle those correctness requirements.

## 7. Experiment B: Capacity growth versus capacity reuse

Every event appends an eight-byte checksum to an audit scratch vector. The baseline reserves room for 4,096 entries before measurement and clears the vector at each batch boundary. The stressed variant releases its storage at each boundary and allows it to grow again.

For `std::vector`, a sufficient `reserve` prevents insertion-triggered reallocation until size would exceed capacity. It does not construct all future elements or guarantee a particular latency. [2]

The harness marks operations where capacity changes. These markers allow us to ask whether slow events coincide with actual growth, rather than blaming growth merely because a vector appears in the code.

This workload intentionally repeats an avoidable scratch-buffer lifecycle. It is not evidence that all vectors repeatedly reallocate in steady state. The relevant lesson is to investigate capacity behavior at the actual lifecycle boundaries of an application.

## 8. Experiment C: Logging and the boundary of completion

The logging variants write the same eight-byte binary record for each event. One uses a 64 KiB stdio buffer; the other additionally calls `fflush` after every event. A no-file-logging baseline helps expose the incremental work.

This comparison asks two different questions: what does adding file logging cost, and what does changing the flush policy cost? Disabling logging changes an application's output requirement. It should not be presented as a free optimization when that record is required.

Buffered logging can defer some work. The harness therefore reports the final flush separately, outside per-event timing. Crucially, `fflush` is not a durability guarantee equivalent to `fsync`; a record reaching the operating system is not the same as surviving a crash. [3]

These tests exclude text formatting, asynchronous logging and durable journal protocols. File destination, filesystem and competing I/O must be recorded with the results. A comparison without those details is difficult to reproduce.

## 9. Scheduler delays: investigate separately from synthetic pauses

The harness includes a diagnostic mode that deliberately busy-waits for at least 50 microseconds every 10,000 events. Its purpose is to check whether the measurement captures a service interruption and the following virtual backlog.

That mode is not a simulation of all OS behavior, and it is not evidence that the scheduler caused an outlier. Its delay is intentionally injected and clearly marked.

Actual scheduler investigation requires separate runs: unpinned versus pinned, and idle versus a documented competing CPU workload. Linux `perf sched` records and analyzes scheduling events; its time-history view can help investigate when a runnable task waited to execute. [4]

Pinning restricts placement; it does not reserve a CPU or eliminate interrupts. Also, a whole-process context-switch count cannot attribute one slow event. Correlating a trace with benchmark samples requires compatible timing boundaries and explicit clock alignment. Profiling adds overhead, so clean timing runs and diagnostic runs should be reported separately.

## 10. The benchmark matrix

| ID | Comparison | Controlled change | Evidence to retain |
|---|---|---|---|
| A | baseline → allocate | Order storage/lifetime strategy | Per-operation service samples and equivalent checksums |
| B | baseline → grow | Scratch-vector capacity lifecycle | Capacity-change markers and nearby latency samples |
| C1 | baseline → log-buffered | Add binary file logging | File bytes, service/response distribution, final flush |
| C2 | log-buffered → log-flush | Per-event flush policy | Same bytes, same destination, latency samples |
| D | baseline → pause | Deliberate periodic interruption | Pause markers and backlog recovery timeline |
| E | baseline under OS configurations | Affinity and documented competing workload | Scheduling traces and repeated clean timing runs |

For each A–D variant, use five independent runs at each of three common offered rates: approximately 25%, 50% and 80% of an instrumented baseline pilot's throughput. These are experimental choices, not claims about production load. Apply the same absolute rate to every mode within a comparison, even if a slower mode becomes overloaded.

Target at least one million completed events per run, extending duration when necessary to observe periodic effects. Randomize variant order to reduce systematic order effects. The supplied suite uses a recorded seed and produces per-run summaries.

## 11. Measurement hygiene and reproducibility

Record CPU topology, RAM, OS/kernel, compiler, standard library, allocator, flags, CPU affinity, power policy, virtualization and log destination. Publish the code revision and exact commands.

The harness pre-generates events and allocates sample storage before timing. It records samples in memory and exports CSV afterward. `steady_clock` supplies monotonic timestamps, but its resolution and call overhead are platform-dependent. Time reads and sample writes are part of the measuring apparatus, not invisible operations.

Order-kernel warm-up is included. Logging first-use behavior and repeated scratch-buffer growth remain visible. If you separate startup from steady state, define the boundary before interpreting the data and report both where relevant.

Do not average several p99.9 values and label the result the pooled p99.9. Show per-run values and variability. If pooling is useful, compute a percentile from pooled raw samples and retain the run-level view.

Finally, inspect more than one visualization: a complementary CDF for service and response, a per-run p99.9 comparison, and a timeline around selected outliers. An aggregate statistic identifies a symptom; temporal evidence helps investigate the mechanism.

## 12. Results: what is established and what is pending

The accompanying validation checks establish that the variants execute equivalent order-state work, produce expected record counts, and preserve the timing identity `response = wait + service`. They also check growth/pause markers and quantile ordering.

They do **not** establish a speedup, an HFT latency budget, or production readiness. Controlled target-machine measurements remain pending.

The publication results table should be populated from the generated CSV, not estimates:

| Comparison | Offered rate | Trials / events | Service p99.9 | Response p99.9 | Observed max | Attribution evidence |
|---|---|---|---|---|---|---|
| A: allocation | Pending | Pending | Pending | Pending | Pending | State transition samples |
| B: growth | Pending | Pending | Pending | Pending | Pending | Capacity markers |
| C: logging | Pending | Pending | Pending | Pending | Pending | Logging/flush configuration |
| D: synthetic control | Pending | Pending | Pending | Pending | Pending | Explicit pause markers |
| E: scheduling | Pending | Pending | Pending | Pending | Pending | Aligned scheduling evidence |

Until that table is populated, this is a methodology article with a runnable companion, not an empirical performance report.

## 13. Optimize an explanation, not just a number

The useful outcome of tail-latency work is an explanation that survives testing. Start with a defined timing boundary, preserve the offered workload, locate the delayed events, and change one relevant mechanism. Then repeat the measurement and document what the change costs in capacity, memory, logging semantics or operational complexity.

A lower p99.9 is valuable. Knowing why it changed, where the result applies, and what it does not guarantee is the stronger engineering contribution.

## References and companion material

1. [HdrHistogram project: coordinated omission and latency recording](https://github.com/HdrHistogram/HdrHistogram).
2. [C++ working draft: vector capacity and reserve](https://eel.is/c++draft/vector.capacity).
3. [Linux man-pages: fflush](https://man7.org/linux/man-pages/man3/fflush.3.html).
4. [Linux perf source documentation: perf sched](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-sched.txt).

Companion files: `README.md`, `src/benchmark.cpp`, `tests/test_benchmark.py`, and `run_suite.py`. Add the actual public repository URL here after you publish the source; none has been invented for this draft.
