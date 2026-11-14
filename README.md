# TitanKV

**High-Performance Persistent In-Memory Key-Value Database Engine**

[![Language](https://img.shields.io/badge/Language-ISO%20C17-blue.svg)](https://en.cppreference.com/w/c/17)
[![Platform](https://img.shields.io/badge/Platform-Linux%20(epoll)%20%7C%20macOS%20%2F%20BSD%20(kqueue)-lightgrey.svg)]()
[![Dependencies](https://img.shields.io/badge/Dependencies-Zero%20(POSIX%20Only)-brightgreen.svg)]()
[![Specification](https://img.shields.io/badge/Spec-IEEE%20Std%20830%20%2F%20ISO%2029148-orange.svg)]()

---

## Overview

**TitanKV** is a standalone, single-node, crash-safe, event-driven key-value storage engine written from scratch in pure **ISO C17** using standard POSIX operating system primitives. Modeled after production systems such as **Redis** and **Bitcask**, TitanKV delivers sub-millisecond in-memory data operations backed by durable, append-only disk persistence—with **zero third-party runtime dependencies**.

### Why TitanKV?
Modern in-memory stores often rely on general-purpose heap allocators (`malloc`/`free`) that fragment over time, or heavy external event libraries. TitanKV solves this at the systems level by combining:
1. **Custom `mmap` Slab & Arena Allocation** to eliminate heap fragmentation and satisfy small allocations in $O(1)$ time.
2. **Incremental Rehashing** to prevent stop-the-world latency spikes as the keyspace grows.
3. **Order-Statistic AVL Trees** paired with hash maps for fast sorted set operations (`ZADD`, `ZRANGE`).
4. **CRC32-Verified Write-Ahead Logging (WAL)** with non-blocking background compaction to guarantee crash safety without stalling client requests.

---

## Key Features

* **Zero External Dependencies:** Built strictly with ISO C17 and POSIX APIs (`epoll`/`kqueue`, `mmap`, `writev`, `fsync`, `pthread`). Compiles cleanly under `-Wall -Wextra -Werror -pedantic`.
* **Custom Slab & Arena Memory Allocator:** Acquires 256 KB virtual memory arenas via `mmap()` and subdivides them into 7 fixed-size intrusive free lists (`32B`, `64B`, `128B`, `256B`, `512B`, `1024B`, `4096B`). Allocations exceeding `4096B` route directly to page-aligned `mmap()` blocks.
* **Incremental Rehashing Hash Table:** Uses 64-bit FNV-1a hashing. Once the load factor exceeds `0.75`, a secondary table of $2\times$ capacity is allocated and up to $K = 8$ non-empty buckets are migrated per request.
* **Sorted Sets (`ZADD`, `ZRANGE`):** Combines an augmented self-balancing AVL tree (`subtree_size` rank tracking) with a member hash map for $O(\log N)$ insertions/updates and $O(\log N + M)$ inclusive rank range queries.
* **Passive & Active TTL Expiration:** Uses an intrusive Min-Heap (`heap_idx` stored directly inside entries) for $O(\log N)$ updates/deletions. Expired keys are evicted lazily on `GET`/`ZRANGE` and actively swept in batches of 16 every `10 ms` on the event loop timer tick.
* **Crash-Safe Persistence & Background Compaction:** Appends every state mutation to `titankv.wal` with an IEEE 802.3 CRC32 checksum. On startup, corrupted or partial tail records (e.g., from `SIGKILL` or power loss) are automatically truncated via `ftruncate()`. When the log exceeds `64 MB`, a background `pthread` writes a deduplicated snapshot to `titankv.wal.tmp`, calls `fsync()`, and atomically replaces the active log via `rename()`.
* **Binary TLV Protocol with Pipelining:** Multiplexes non-blocking TCP sockets over `epoll` (Linux) or `kqueue` (macOS/FreeBSD), executing multiple concatenated client commands per `read()` syscall and coalescing responses.

---

## Architecture

```text
+-------------------------------------------------------------------+
|                        Client Connections                         |
|                  (TCP / Binary TLV Pipelining)                    |
+---------------------------------+---------------------------------+
                                  | Non-Blocking Sockets
                                  v
+-------------------------------------------------------------------+
|                   Event Loop (epoll / kqueue)                     |
|  - Connection State Machine (Dynamic Read/Write Buffers)          |
|  - 10 ms Periodic Timer Tick (Active TTL Sweeper + fsync Flush)   |
+---------------------------------+---------------------------------+
                                  | Decoded Command
                                  v
+-------------------------------------------------------------------+
|                        Execution Engine                           |
|  +-----------------------+  +----------------+  +--------------+  |
|  | Incremental HashTable |  |  AVL / ZSet    |  |   Min-Heap   |  |
|  +-----------------------+  +----------------+  +--------------+  |
|                                 |                                 |
|                 Backed by Custom Slab/Arena Allocator             |
+---------------------------------+---------------------------------+
                                  | Mutating Operations
                                  v
+-------------------------------------------------------------------+
|                     Persistence Subsystem                         |
|  - Append-Only WAL Writer (CRC32 + Scatter-Gather writev)         |
|  - Background Compaction Thread (pthread + atomic rename)         |
+-------------------------------------------------------------------+