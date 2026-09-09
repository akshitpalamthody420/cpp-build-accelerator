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

The current client connects to `127.0.0.1:9000`, matching the WSL + Windows localhost + VMware NAT forwarding setup used during development.

## Cache test

Run the same build twice. The first build should log `CACHE MISS`; the second should log `CACHE HIT`.

Change a header or a compiler flag and the affected job should become a cache miss.

## Features that I am going to implement next

- Multiple remote workers and round-robin/load-aware scheduling
- Retry/failover on worker failure
- Compiler stderr/stdout forwarding
- Socket timeouts and protocol hardening
- Temporary workspace cleanup
- Benchmarks across 1/2/3 workers and warm-cache builds
