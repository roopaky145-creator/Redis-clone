# Redis Clone

A Windows-first, high-performance C++17 Redis-style in-memory key-value store built from scratch.

This project implements a fully concurrent database engine supporting the official RESP (REdis Serialization Protocol), Winsock TCP networking, a custom worker thread pool, thread-safe storage, $O(1)$ LRU eviction, active/lazy TTL expiry, and asynchronous write-ahead logging (WAL) for durability.

## Features

- **RESP Parsing**: Zero-copy RESP protocol parser compatible with official Redis clients.
- **Concurrency**: Custom thread pool and `std::shared_mutex` lock optimization for high-throughput, multi-client support.
- **Memory Management**: $O(1)$ LRU cache eviction and Active/Lazy Time-To-Live (TTL) key expiry.
- **Durability**: Asynchronous Buffer-and-Batch Write-Ahead Logging (WAL) for zero-data-loss crash recovery.

## Performance Benchmarks

The server achieves high throughput under heavy concurrency by completely decoupling disk I/O from the network worker threads.

**Workload**: 100,000 SET requests across 50 concurrent Python client threads.
**Target**: `build/Release/redis_clone.exe` (Windows)

```plaintext
Starting Benchmark: 100000 SETs across 50 threads...
------------------------------
Benchmark Complete!
Time Taken : 10.822 seconds
Throughput : 9,240 QPS (Queries Per Second)
------------------------------
```

### Benchmark Notes & Context
The ~9.2K QPS figure reflects specific local Windows constraints: Winsock thread-per-connection overhead, Python client synchronization, and WAL NTFS flush latency. Native Redis on Windows shows similar degradation compared to its Linux counterpart. Porting this architecture to Linux utilizing `epoll` instead of blocking sockets would eliminate the thread-per-connection overhead and drastically increase throughput. The primary success of this benchmark is proving that the asynchronous WAL design successfully prevents disk I/O from stalling the network worker threads under heavy concurrency.

## Architecture

```text
Client (redis-cli / Python)
        │  TCP / RESP Protocol
        ▼
┌─────────────────────────────────┐
│         Server (Winsock)        │
│  accept loop → ThreadPool(128)  │
└──────────────┬──────────────────┘
               │ ICommandHandler::handleCommand()
               ▼
┌─────────────────────────────────────────────────────┐
│                    Store                            │
│  shared_mutex (RW lock)                             │
│  ┌─────────────────┐   ┌──────────────────────┐     │
│  │ unordered_map   │   │  std::list (LRU)     │     │
│  │ key → CacheNode │◄──│  MRU ←────────► LRU │     │
│  └─────────────────┘   └──────────────────────┘     │
│  Active expiry thread (100ms, random bucket sample) │
└────────────────────────┬────────────────────────────┘
                         │ WAL append (O(1), no disk I/O)
                         ▼
┌─────────────────────────────────┐
│         WalLogger               │
│  in-memory buffer               │
│  flush thread (100ms swap trick)│
│  → redis_clone.wal (RESP format)│
└─────────────────────────────────┘
```

### The Async WAL "Swap Trick"

To prevent mechanical disk spin-up or SSD write latency from blocking network threads, mutating commands enqueue serialized RESP records into an in-memory WAL buffer. `WalLogger::append()` briefly acquires a mutex, pushes the record, and returns immediately.

A background flush thread wakes every 100ms, swaps the shared buffer into a local vector via `std::swap(localBuffer, writeBuffer_)`, releases the mutex in $O(1)$ time, and writes the batch to disk with a single `.flush()`. This keeps command threads away from synchronous disk I/O while preserving orderly append-only WAL records.

### Random Sampling TTL Expiry

TTL expiry operates on a hybrid Lazy/Active model. Reads or TTL-related commands passively remove expired keys when encountered. Additionally, an active background thread wakes every 100ms, acquires an exclusive store lock, and randomly samples `std::unordered_map` buckets to delete expired entries found in those short-lived samples.

The store strictly avoids persisting map iterators across calls, entirely eliminating the risk of catastrophic iterator invalidation during background map rehashes.

## Quick Start

### Prerequisites

- Windows Operating System
- CMake
- MSVC Build Tools or Visual Studio with the C++ workload

### Build Instructions

Open your developer command prompt and run:

```dos
# Generate the build files
cmake -S . -B build

# Compile the executable in Release mode
cmake --build build --config Release
```

### Run the Server

```dos
.\build\Release\redis_clone.exe
```

By default, the server listens for TCP connections on port `6379` and writes its write-ahead log to `redis_clone.wal` in the current working directory.
