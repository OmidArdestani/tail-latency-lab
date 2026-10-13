# Validation record

Date: 2026-10-13.

- Compiler: g++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
- OS: Linux-6.18.44-x86_64-with-glibc2.39
- Optimized C++20 build completed with -Wall -Wextra -Wpedantic and no warnings.
- Three Python test cases passed, covering all six variants plus built-in state/quantile checks, invalid inputs and unpaced replay.
- Repeated-suite smoke run completed: six variants, two repetitions, 100,000 events per run, 1,000 ns scheduled interval.
- All 12 runs produced the same checksum: 12394744752.
- All 36 summary rows were readable; raw CSV files were produced successfully.

These are functional/smoke checks in a shared execution environment, not controlled publication benchmarks. No numeric performance claims are derived from them. Scheduling traces and target-machine trials have not been performed.

The ZIP intentionally excludes generated binaries, raw timing data and machine-specific smoke results. Recreate them with the README commands.
