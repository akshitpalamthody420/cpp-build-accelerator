# Local benchmark results

Measured on September 16, 2026. The raw samples and environment information are
in [benchmark-results.json](benchmark-results.json).

| Build | Run 1 | Run 2 | Run 3 | Median |
| --- | ---: | ---: | ---: | ---: |
| Local GCC, sequential | 0.427 s | 0.431 s | 0.436 s | 0.431 s |
| Forge, 4 jobs, empty cache | 0.389 s | 0.386 s | 0.386 s | 0.386 s |
| Forge, 4 jobs, repeated cached build | 0.181 s | 0.183 s | 0.181 s | 0.181 s |

The median cold Forge build was about 1.12x as fast as the sequential baseline.
The cached build was about 2.37x as fast. These ratios use the unrounded samples.

## Setup

- WSL2 Linux, x86-64, 24 reported logical CPUs.
- GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1).
- Client and worker on the same machine, connected over loopback.
- Twenty tiny source files from `pool-test`, plus a generated main: 21 files total.
- All modes used `-std=c++20 -O2`, compiled separate objects, and linked locally.
- Three repetitions, four client jobs for both Forge modes.

The timer covers compilation and linking. It does not include building Forge,
starting the worker, or running the resulting executable to verify its output.
Each trial uses a fresh worker directory. The first Forge build must report
21 compiled objects and zero hits; the repeated build must report 21 hits and
zero newly compiled objects. Every executable must print `210`.

The order is always local sequential, cold Forge, then cached Forge. The OS
filesystem cache is not cleared, so “cold” refers only to Forge's object cache.
Files are copied into a Linux temporary directory before the timed runs.

## What this tells us

The cached build avoided compilation and was faster on this workload. The cold
build had a smaller improvement: these source files do very little, so GCC
startup, dependency discovery, hashing, and transfer overhead take a noticeable
part of the total time.

This is not a comparison against local parallel Make or Ninja, and it does not
measure network latency between separate machines. Three short runs also leave
room for normal timing noise. Use a larger project and more repetitions before
drawing conclusions about a production build.

To run it again:

```bash
make benchmark REPEATS=3 JOBS=4
```

That updates the JSON results. This table records the run above; it is not
automatically rewritten when the benchmark is rerun.
