# C++20 M:N Coroutine Pool Design

**Date:** 2026-03-29
**Author:** Claude
**Status:** Draft

## Overview

Implement a C++20 coroutine-based M:N thread pool for general-purpose task scheduling. This is Phase 1 of a two-phase project; Phase 2 will add IO integration (async socket/file operations).

## Goals

- Provide modern C++20 coroutine API with async/await style
- M:N scheduling model: M coroutines mapped to N worker threads
- Work-stealing for load balancing across workers
- Pool-based coroutine frame allocation for performance
- Coroutine-specific synchronization primitives
- Support for cooperative cancellation

## Non-Goals (Phase 1)

- IO-driven scheduling (socket/file async operations) - Phase 2
- Integration with epoll/IOCP - Phase 2
- Priority-based scheduling
- Hard real-time guarantees

## Architecture

### Layer Structure

```
┌─────────────────────────────────────────────────┐
│                   User API                       │
│  Task<T>, SafeTask<T>, co_spawn(), co_await     │
├─────────────────────────────────────────────────┤
│              Coroutine Scheduler                 │
│  CoroutineMeta Pool, Frame Allocator, Dispatch  │
├──────────────────────┬──────────────────────────┤
│   Shared Infrastructure (from bthread)          │
│  Worker Pool, WorkStealingQueue, GlobalQueue    │
└──────────────────────┴──────────────────────────┘
```

### Components

| Component | Responsibility |
|-----------|----------------|
| `CoroutineMeta` | Coroutine metadata: state, promise pointer, owner Worker, cancellation flag |
| `FramePool` | Pre-allocated coroutine frame memory pool, custom `operator new` for promise_type |
| `CoroutineScheduler` | Manage coroutine lifecycle, dispatch to Workers |
| `Task<T>` | User coroutine return type, exception-based error handling |
| `SafeTask<T>` | User coroutine return type, `Result<T>` based error handling |
| `CoMutex` | Coroutine mutex, `co_await` suspend instead of blocking Worker |
| `CoCond` | Coroutine condition variable, `co_await` wait |

### Relationship with bthread

- **Reuse:** Worker pool, WorkStealingQueue, GlobalQueue infrastructure
- **Independent:** CoroutineMeta (separate from TaskMeta), frame allocation logic, coroutine-specific sync primitives
- Workers execute both bthread tasks and coroutines, differentiated by task type flag

## Detailed Design

### CoroutineMeta

```cpp
struct CoroutineMeta {
    enum State { READY, RUNNING, SUSPENDED, FINISHED };

    std::coroutine_handle<> handle;
    State state;
    Worker* owner_worker;
    std::atomic<bool> cancel_requested;
    void* waiting_sync;  // Pointer to CoMutex/CoCond if waiting
    CoroutineMeta* next; // For queue linkage
    uint32_t slot_index;
    uint32_t generation;
};
```

### FramePool

Pre-allocate fixed-size memory blocks for coroutine frames:

- Two size tiers: 4KB (small coroutines), 8KB (larger coroutines)
- Pool managed as thread-safe free lists
- Custom `promise_type::operator new` delegates to pool
- On coroutine completion, frame memory returned to pool

### Task<T> and SafeTask<T>

**Task<T>:**
```cpp
template<typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;

    bool await_ready();
    void await_suspend(std::coroutine_handle<> awaiting);
    T await_resume();  // Throws if coroutine threw exception

private:
    std::coroutine_handle<TaskPromise<T>> handle_;
};
```

**SafeTask<T>:**
```cpp
template<typename T>
class SafeTask {
public:
    using promise_type = SafeTaskPromise<T>;

    Result<T> await_resume();  // Returns Result<T>, no exception

private:
    std::coroutine_handle<SafeTaskPromise<T>> handle_;
};
```

### Result<T>

```cpp
template<typename T>
class Result {
public:
    bool is_ok() const;
    bool is_err() const;
    T& value();
    const T& value() const;
    Error& error();
    const Error& error() const;

private:
    std::variant<T, Error> data_;
};
```

### CoMutex

```cpp
class CoMutex {
public:
    Awaitable<void> lock();    // co_await mutex.lock()
    void unlock();

private:
    std::atomic<uint32_t> state_;  // LOCKED flag, WAITERS count
    CoroutineQueue waiters_;       // Queue of waiting coroutines
};
```

Lock algorithm:
1. Try atomic CAS to acquire lock
2. If failed, add self to waiter queue, suspend coroutine
3. On unlock: if waiters exist, wake one coroutine

### CoCond

```cpp
class CoCond {
public:
    Awaitable<void> wait(CoMutex& mutex);
    void signal();
    void broadcast();

private:
    CoroutineQueue waiters_;
};
```

### Cancellation

```cpp
class CancellationToken {
public:
    void request_cancel();
    bool is_cancelled() const;
    Awaitable<bool> check_cancel();  // co_await token.check_cancel()
};

class CancelSource {
public:
    CancellationToken token();
    void cancel();
};
```

Coroutines check cancellation at explicit `co_await` points. No forced termination.

### Scheduler Dispatch Flow

1. `co_spawn(task)`:
   - Allocate CoroutineMeta from pool
   - Allocate frame from FramePool (via promise_type::operator new)
   - Initialize coroutine handle
   - Push to caller Worker's local queue (or GlobalQueue if no Worker context)

2. Worker execution loop:
   - Pop from local WorkStealingQueue (or steal from others)
   - If coroutine: call `handle.resume()`
   - If bthread task: execute existing bthread logic

3. Coroutine suspend (`co_await`):
   - Coroutine returns from `resume()`
   - Scheduler examines suspend reason (yield, sync primitive, sleep)
   - Store waiting state in CoroutineMeta
   - CoroutineMeta queued appropriately (sync primitive's waiter list, timer, or ready queue)

4. Coroutine resume:
   - Condition satisfied (mutex available, timer expired, signal received)
   - CoroutineMeta moved back to Worker queue
   - Worker resumes `handle.resume()`

5. Coroutine finish:
   - `promise_type::return_value()` stores result
   - CoroutineMeta state = FINISHED
   - Wake any awaiting coroutines
   - Frame memory returned to pool

### co_spawn and co_await API

```cpp
// Spawn a coroutine (returns Task<SafeTask>)
template<typename T>
Task<T> co_spawn(Task<T> task);

// Spawn in detached mode (fire-and-forget)
template<typename T>
void co_spawn_detached(Task<T> task);

// Explicit yield point
Awaitable<void> yield();

// Sleep (for testing/demo)
Awaitable<void> sleep(std::chrono::milliseconds duration);
```

## File Structure

```
include/coro/
  coroutine.h         # Task<T>, SafeTask<T>, co_spawn
  scheduler.h         # CoroutineScheduler
  mutex.h             # CoMutex
  cond.h              # CoCond
  frame_pool.h        # FramePool
  result.h            # Result<T>
  cancel.h            # CancellationToken, CancelSource
  meta.h              # CoroutineMeta

src/coro/
  coroutine.cpp       # Task/SafeTask promise implementations
  scheduler.cpp       # CoroutineScheduler implementation
  mutex.cpp           # CoMutex implementation
  cond.cpp            # CoCond implementation
  frame_pool.cpp      # FramePool implementation
  cancel.cpp          # Cancellation implementation
```

## Tests

Tests will cover:

1. Basic coroutine creation and completion
2. `co_await` Task/SafeTask results
3. Error handling (exception vs Result)
4. Work stealing across Workers
5. CoMutex contention
6. CoCond wait/signal
7. Cancellation flow
8. FramePool allocation/deallocation
9. Detached coroutines
10. Nested coroutines
11. Concurrent spawn stress test

## Performance Considerations

- FramePool reduces heap allocation overhead
- WorkStealingQueue is already optimized (from bthread)
- CoroutineMeta pool reduces allocation for frequent spawn/destroy
- CoMutex waiters queue uses intrusive linkage (no extra allocation)

## Platform Support

- Primary: Windows x64 (MSVC 2022, C++20)
- Secondary: Linux x64 (GCC 10+, Clang 10+)
- Coroutine frame allocation is platform-agnostic (standard C++20)

## Dependencies

- C++20 compiler (coroutine support required)
- Existing bthread infrastructure (Worker, WorkStealingQueue, GlobalQueue)
- No external library dependencies

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Coroutine frame size varies | Two-tier pool (4KB/8KB), fallback to heap for oversized frames |
| Worker starvation if many coroutines suspend | GlobalQueue as fallback, ensure ready coroutines can be stolen |
| Cancel request ignored by coroutine | Document: cancellation is cooperative, user must check at await points |
| Exception safety in await_resume | Task<T> propagates exception; SafeTask<T> catches and returns Result |

## Phase 2 Preview (IO Integration)

Phase 2 will extend this design with:

- `AsyncSocket` class with `co_await read()/write()`
- Integration with platform IO (IOCP on Windows, epoll on Linux)
- IO-driven coroutine wake-up
- Async file operations

This will require adding an IO poller thread and extending CoroutineMeta with IO wait state.