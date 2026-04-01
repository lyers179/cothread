# bthread Architecture

## Overview

bthread is an M:N threading library that multiplexes M user-space tasks onto N OS threads. It supports two execution models:

1. **bthread**: Traditional user-space threading with assembly-based context switching
2. **coroutine**: C++20 stackless coroutines with compiler-managed context

## Core Components

### TaskMetaBase (include/bthread/core/task_meta_base.hpp)

The unified base class for all task types:

```cpp
struct TaskMetaBase {
    std::atomic<TaskState> state;      // READY, RUNNING, SUSPENDED, FINISHED
    TaskType type;                      // BTHREAD or COROUTINE
    std::atomic<TaskMetaBase*> next;   // Queue linkage
    void* waiting_sync;                 // Sync primitive if waiting
    uint32_t slot_index;                // Task pool slot
    uint32_t generation;                // For ID encoding

    virtual void resume() = 0;          // Resume execution
};
```

### TaskMeta (include/bthread/task_meta.h)

bthread-specific metadata:

- Stack and context (assembly-level)
- Entry function (fn/arg/result)
- Reference counting for join/detach
- Waiter state for Butex synchronization

### CoroutineMeta (include/coro/meta.h)

Coroutine-specific metadata:

- `std::coroutine_handle<>` for execution
- Cancellation support

### Scheduler (include/bthread/scheduler.h)

Unified scheduler that manages both task types:

```cpp
class Scheduler {
    void Submit(TaskMetaBase* task);    // Unified submission
    void EnqueueTask(TaskMeta* task);  // bthread-specific
    template<typename T>
    Task<T> Spawn(Task<T> task);       // Coroutine-specific
};
```

### Worker (include/bthread/worker.h)

Worker threads that execute tasks:

```cpp
void Worker::Run() {
    while (running_) {
        TaskMetaBase* task = PickTask();
        switch (task->type) {
            case TaskType::BTHREAD:
                RunBthread(static_cast<TaskMeta*>(task));
                break;
            case TaskType::COROUTINE:
                RunCoroutine(static_cast<CoroutineMeta*>(task));
                break;
        }
    }
}
```

## Synchronization Primitives

### Mutex (include/bthread/sync/mutex.hpp)

Unified mutex with dual-mode locking:

- `lock()` / `unlock()`: Blocking operations for bthread/pthread
- `lock_async()`: Returns awaiter for coroutines
- `try_lock()`: Non-blocking attempt

Implementation:
- Uses `Butex` for bthread waiting (futex-based)
- Uses `SRWLOCK`/`pthread_mutex` for pthread waiting
- Waiter queue for coroutine waiting

### CondVar (include/bthread/sync/cond.hpp)

Unified condition variable:

- `wait(Mutex&)`: Blocking wait
- `wait_async(Mutex&)`: Awaitable wait
- `wait_for()`: Timed wait
- `notify_one()` / `notify_all()`: Wake waiters

### Event (include/bthread/sync/event.hpp)

Simple binary event:

- `wait()` / `wait_async()`: Wait for event
- `set()`: Wake all waiters
- `reset()`: Clear event
- Auto-reset mode support

## Task Queues

### GlobalQueue (include/bthread/global_queue.h)

MPSC (Multi-Producer Single-Consumer) queue:

- Lock-free push from any thread
- Single consumer pop
- FIFO ordering

### WorkStealingQueue (include/bthread/work_stealing_queue.h)

Lock-free double-ended queue:

- Owner pushes/pops from tail (LIFO)
- Thieves steal from head (FIFO)
- ABA prevention with version counters

## Context Switching

### bthread (Assembly)

Platform-specific assembly code saves/restores CPU registers:

```asm
; Windows x64
SwapContext:
    ; Save callee-saved registers
    push rbx, rbp, rsi, rdi, r12-r15
    ; Save XMM registers
    ; Save stack pointer
    ; Switch to new context
    ; Restore XMM registers
    ; Restore callee-saved registers
    ret
```

### Coroutine (C++20)

Compiler generates state machine:

- Stackless (no separate stack)
- State stored in coroutine frame
- `co_await` suspends/resumes

## Memory Management

### TaskMeta Pool

Fixed-size pool (16384 slots) with generation counters:

```cpp
class TaskGroup {
    std::atomic<TaskMeta*> task_pool_[POOL_SIZE];
    std::atomic<uint32_t> generations_[POOL_SIZE];
};
```

### Coroutine Frame Pool

Frame pool for coroutine promise allocations:

```cpp
class FramePool {
    void Init(size_t block_size, size_t block_count);
    void* Allocate(size_t size);
    void Deallocate(void* ptr);
};
```

## Threading Model

```
+------------------+    +------------------+    +------------------+
|    Worker 0      |    |    Worker 1      |    |    Worker N      |
+------------------+    +------------------+    +------------------+
        |                       |                       |
        v                       v                       v
+------------------+    +------------------+    +------------------+
|  Local Queue     |    |  Local Queue     |    |  Local Queue     |
| (WorkStealingQ)  |    | (WorkStealingQ)  |    | (WorkStealingQ)  |
+------------------+    +------------------+    +------------------+
        \                       |                       /
         \                      |                      /
          \---------------------+---------------------/
                               |
                               v
                    +-------------------+
                    |   Global Queue    |
                    |    (MPSC Queue)   |
                    +-------------------+
```

Workers:
1. Check local queue first
2. Check global queue
3. Steal from random victim worker

## Platform Abstraction

### Futex Operations

- Linux: `futex()` syscall
- Windows: `WaitOnAddress()` / `WakeByAddressSingle()` / `WakeByAddressAll()`

### Stack Allocation

- Allocate with guard page for overflow detection
- Platform-specific allocation (`VirtualAlloc` on Windows, `mmap` on Linux)

### Thread Creation

- Windows: `_beginthreadex()`
- Linux: `pthread_create()`