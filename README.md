# Forge — Distributed C++ Build Accelerator

Forge is a C++20 remote build accelerator prototype. The client discovers C/C++ project dependencies using GCC, transfers each compilation job over TCP, and receives compiled object files from a Linux worker.

## Implemented

- TCP client/worker protocol using POSIX sockets
- Parallel compilation requests from the client
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
