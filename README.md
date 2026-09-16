# Forge — C++ build accelerator

Forge sends C++ compilation jobs to a Linux worker over TCP, then links the
returned object files on the client. The worker keeps an object cache so it can
skip compilation when the source, project headers, flags, and compiler identity
match an earlier job.

This is a small single-worker project. It uses GCC for the actual compilation;
Forge handles the file transfer, scheduling, cache lookup, and build results.

## Build it

Use Linux or WSL with a GCC version that supports C++20, including `std::jthread`
and `std::osyncstream`. You also need Make and coreutils (`sha256sum` and `head`).
Python 3 is only needed for the tests and benchmark.

On Ubuntu or Debian:

```bash
sudo apt update
sudo apt install build-essential python3 coreutils
make
```

Run `make` on both machines when using a separate worker. The client also needs
GCC: it discovers headers locally and performs the final link. Use compatible
GCC toolchains, target architectures, and system headers on both machines.

`make` builds `forge-client` and `forge-worker`. Compiler settings can be changed
with commands such as `make CXX=g++ CXXFLAGS='-O0 -g -Wall'`. The test scripts use
`g++` directly so their compiler choice is explicit.

## Run a build

Start the worker in one terminal:

```bash
./forge-worker --port 9000
```

In another terminal, run the client from the directory containing your sources:

```bash
./forge-client -j 4 -o app main.cpp math.cpp strings.cpp
./app
```

The defaults are `127.0.0.1:9000`, four concurrent jobs, and an executable named
`app`. To use another machine:

```bash
./forge-client --host 192.168.1.50 --port 9000 -j 4 -o app main.cpp math.cpp strings.cpp
```

The host must be an IPv4 address. The worker listens on all interfaces. Use a
trusted network: the protocol has no authentication or encryption, and the
compiler is not sandboxed.

Put Forge options first. If you need compiler flags, separate them from the
source list with `--`:

```bash
./forge-client -o flag-app -std=c++20 -O2 -Iflag-test/include -DFACTOR=7 -- flag-test/src/main.cpp
./forge-client --compile-only -j2 main.cpp math.cpp strings.cpp
./forge-client -o app --link-flag -lm -std=c++20 -O2 -- main.cpp math.cpp strings.cpp
```

- `-j N` or `-jN` sets the client job limit, from 1 to 256. `-j1` is serial.
- `-o NAME` chooses the executable name.
- `--compile-only` saves objects under `returned/` without linking.
- `--link-flag ARG` adds one linker argument after the object files. Repeat it
  for more arguments. Compiler flags are also used during linking, so options
  such as `-pthread` and `-fsanitize=address` carry through.

A failed compilation skips linking. A failed link leaves an existing executable
in place, so check the command's exit status before running an old executable.
The client returns 0 on success and 1 on a build failure.

Compiler errors and warnings appear on the client with their source locations
and compiler exit status. Output is capped at 1 MiB per job. Cached objects do
not replay earlier warnings. Rebuild and restart the client and worker together
when updating Forge; the protocol is not version-negotiated.

## How the pieces fit together

```text
source list -> client job threads -> TCP -> worker queue -> cache lookup
                                                  | miss: run g++
returned objects <- TCP <--------------------------+
       |
       +-> local g++ link -> executable
```

`forge-client.cpp` runs `g++ -MM` to find each source's project headers, including
indirect includes. It sends those files, their relative paths, and the compiler
flags to the worker. A fixed number of client threads takes jobs from the source
list; finishing one job frees that thread for the next one.

`forge-worker.cpp` has a thread pool based on its reported hardware concurrency
and a queue of up to 64 waiting connections. Each job gets a fresh directory.
The cache key hashes the compiler identity, flags in order, source path, and
sorted dependency paths and contents. A hit returns the stored object; a miss
runs GCC and publishes the object in the cache.

The worker explicitly marks cached results. The client counts successfully
received fresh objects, cached objects, and failed jobs, and reports link status
and elapsed time. Full builds use a private object directory, so an object left
by a previous build cannot accidentally be linked. Temporary job directories are
removed when their jobs finish. `job-support.h` holds the shared resource helpers.

## Tests

```bash
make test
```

The scripts compile their own test binaries and use temporary directories and
spare ports. They do not need your normal worker to be running.

`test-failures.py` checks disconnects, exceptions, failed writes, compiler errors,
link failures, cache counts, output preservation, and the measured number of
concurrent requests for `-j1`, `-j3`, and the default limit.

`test-examples.py` builds and runs the small programs in this repository:

- The basic math and greeting program.
- `dependency-test`: indirect headers and cache invalidation after a header edit.
- `flag-test`: include directories and a changed macro definition.
- `path-test` and `complex-test`: nested paths and duplicate source basenames.
- `pool-test`: twenty source files, linked with a generated main that checks the sum.

Only temporary copies of the examples are edited during testing. Assertions
check program output and relevant cache counts; any failure stops the test.

## Benchmark

```bash
make benchmark
make benchmark REPEATS=5 JOBS=2
```

This compares ordinary sequential GCC compilation, a parallel Forge build with
an empty object cache, and the same Forge build repeated with a populated cache.
It uses the twenty tiny `pool-test` files plus a generated main, with C++20 and
`-O2`. Each executable is run to check its output.

The script records every sample, medians, compiler, OS, CPU count, and job limit
in `benchmark-results.json`. It times the complete compile-and-link command,
excluding tool compilation, worker startup, and executable verification. Each
trial gets a fresh worker directory; the OS filesystem cache is not cleared.
Client and worker run on the same machine over loopback. The local baseline runs
first, then the cold Forge build, then the warm build.

See [BENCHMARK.md](BENCHMARK.md) for the checked-in measurements. These examples
are small enough that process startup and transfer overhead matter a lot. They
are useful for checking behavior, but do not establish a speedup for large
projects or a real network of machines.

## Current limits

- One worker per client; no discovery, load balancing, or automatic retry.
- GCC and Linux/POSIX only. No native Windows/MSVC support or cross-compilation setup.
- Sources and project headers must use relative paths inside the project. GCC
  dependency output with spaces or escaped characters in filenames is not parsed
  correctly yet. Select source files explicitly; Forge does not scan directories.
- System headers are not transferred or hashed. Changes to those headers or
  environmental inputs can make the object cache stale. Time-dependent macros
  are another cache limitation.
- Only simple compilation flags are supported. Options that change GCC's output
  mode, response files, modules, and generated-file build graphs are not handled.
- The client limit applies to one invocation. Several clients can still fill the
  worker queue. Socket operations have a 120-second timeout, but a hung compiler
  process has no execution deadline yet.
- The cache has no size limit or eviction policy. Stop the worker before using
  `make clean`, which removes binaries, cached objects, and job/output directories.
