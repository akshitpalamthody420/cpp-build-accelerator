# Forge — Distributed C++ Build Accelerator

Forge is a C++20 remote build accelerator prototype. The client discovers C/C++ project dependencies using GCC, transfers each compilation job over TCP, and receives compiled object files from a Linux worker.

## Implemented

- TCP client/worker protocol using POSIX sockets
- Bounded parallel compilation requests from the client (`-j`)
- Relative-path preservation for nested project trees
- GCC `-MM` direct/transitive project-header discovery
- Transfer of source files, headers, and compiler flags
- Per-job isolated worker directories
- SHA-256 content-addressed object cache
- Cache invalidation on source/header contents, compiler flags, source path, and compiler identity
- Bounded worker thread pool with a 64-job queue
- Configurable worker IPv4 address and TCP port
- Compiler output and exit-status forwarding
- Socket timeouts, per-job failure handling, and temporary workspace cleanup
- Local executable linking and per-invocation build statistics

## Build

Linux / WSL:

```bash
make
```

Or manually:

```bash
g++ -std=c++20 forge-client.cpp -o forge-client -pthread
g++ -std=c++20 forge-worker.cpp -o forge-worker -pthread
```

The worker also expects `sha256sum` to be available.

## Run

Start the worker:

```bash
./forge-worker
```

Run the client from the project root:

```bash
./forge-client main.cpp math.cpp strings.cpp
```

This compiles the selected files and links them locally into `app`. Choose an
output name with `-o`, or keep the earlier object-only workflow with
`--compile-only`:

```bash
./forge-client -o my-app main.cpp math.cpp strings.cpp
./forge-client -j 2 -o my-app main.cpp math.cpp strings.cpp
./forge-client --compile-only main.cpp math.cpp strings.cpp
./forge-client -o my-app --link-flag -lm -std=c++20 -O2 -- main.cpp math.cpp strings.cpp
```

Place Forge options before compiler flags or source files. Repeat `--link-flag`
for multiple linker arguments. Compiler flags are also supplied to local GCC at
link time, which preserves options such as `-pthread` and `-fsanitize=address`.
Link-only arguments are added after the object files.

Use `-j N` or `-jN` to limit simultaneous client compilation jobs. The default is
4; accepted values are 1 through 256, capped by the number of selected files.
The limit covers dependency discovery, connection, transfer, and result handling.
As soon as one job finishes, its thread takes the next source; a failed job does
not prevent later sources from being processed. `-j1` processes sources serially.
The worker's own thread pool still controls how many compilers run on its machine.
This limit applies per client invocation; an overloaded worker can still reject
requests from multiple clients, and automatic retries are not implemented yet.

Complete builds use fresh object directories and link only the objects received
for that invocation. Any compilation failure skips linking. The executable is
published only after a successful link, so a failed build leaves an existing
executable unchanged. Temporary build objects are removed afterward;
`--compile-only` retains objects under `returned/` as before.

Every completed build invocation prints a summary such as:

```text
Build summary: compiled=2, cache hits=1, failures=0, link=succeeded, elapsed=0.431s
```

`compiled` counts successfully received newly compiled objects; `cache hits`
counts successfully received cached objects, explicitly identified by the
worker's cached-result response. `failures` counts failed compilation jobs,
including transport or local object-write failures. Link failure is reported
separately and also makes the client exit unsuccessfully. Elapsed time includes
dependency discovery, transfer, compilation/cache lookup, and local linking.

With compiler flags:

```bash
./forge-client -std=c++20 -O2 -Iflag-test/include -DFACTOR=7 -- flag-test/src/main.cpp
```

The default endpoint is `127.0.0.1:9000`. To use another machine or port:

```bash
# On the worker machine:
./forge-worker --port 9100

# On the client machine (replace the example IP with your worker's IPv4 address):
./forge-client --host 192.168.1.50 --port 9100 main.cpp math.cpp strings.cpp
./forge-client --host 192.168.1.50 --port 9100 -std=c++20 -Wall -- main.cpp
```

Place connection options before compiler flags or source files. Ports must be
between 1 and 65535. Hostnames and IPv6 are not supported yet. The worker listens
on all interfaces; use it on a trusted network with a compatible GCC toolchain.

Compiler errors and warnings appear on the client's stderr, grouped by source
file with the compiler exit status. The client exits with 1 if any job fails and
0 if all succeed. A compiler killed by a signal is reported as 128 plus its signal
number. Output is limited to 1 MiB per job, with a truncation notice. Cached jobs
return the object without replaying earlier warnings. Dependency-discovery errors
are still reported directly by the client's local GCC.

Rebuild and restart both programs together: the compilation response format now
includes exit status and diagnostics. Older clients cannot read the new responses.

Run the Linux regression tests with `python3 test-failures.py`. They compile both
programs and run on a spare port without modifying the source or existing worker.

## Cache test

Run the same build twice. The first build should log `CACHE MISS`; the second should log `CACHE HIT`.

Change a header or a compiler flag and the affected job should become a cache miss.

## Features that I am going to implement next

- Multiple remote workers and round-robin/load-aware scheduling
- Retry/failover on worker failure
- Further protocol hardening
- Benchmarks across 1/2/3 workers and warm-cache builds
