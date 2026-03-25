# M:N Thread Pool Design

**Date**: 2026-03-24
**Status**: Draft
**Author**: Design Session

## Overview

This document describes the design of a high-performance M:N thread pool implementation, based on the bthread architecture used in brpc. The "M:N" model maps M user-level threads (bthreads) to N POSIX threads (pthreads), where M >> N, enabling efficient concurrency with synchronous programming style.

## Goals

- Support synchronous programming model
- Create bthreads in hundreds of nanoseconds
- Provide multiple synchronization primitives
- All APIs callable from pthread with reasonable behavior
- Fully utilize multiple cores
- Better cache locality

## Non-Goals

- Full pthread API compatibility (explicit API usage required)
- Hook all blocking glibc functions and syscalls
- Kernel modifications for fast context switching

## Architecture

### Directory Structure

```
bthread/
├── include/
│   └── bthread.h              # Public API header
├── src/
│   ├── bthread.cpp            # bthread create/manage
│   ├── task_meta.cpp          # TaskMeta definition
│   ├── task_group.cpp         # TaskMeta pool management
│   ├── worker.cpp             # Worker thread implementation
│   ├── scheduler.cpp          # Scheduler core logic
│   ├── global_queue.cpp       # Global task queue
│   ├── work_stealing_queue.cpp # Work Stealing queue
│   ├── butex.cpp              # Butex synchronization
│   ├── timer_thread.cpp       # Timer thread
│   ├── execution_queue.cpp    # ExecutionQueue implementation
│   └── platform/
│       ├── platform.h         # Platform abstraction interface
│       ├── platform_linux.cpp # Linux implementation
│       └── platform_windows.cpp # Windows implementation
└── tests/
    └── *_test.cpp             # Unit tests
```

### Module Responsibilities

| Module | Responsibility |
|--------|---------------|
| TaskMeta | bthread metadata (stack, state, context, wait state) |
| TaskGroup | TaskMeta pool and bthread_t mapping |
| Worker | pthread worker thread executing bthreads |
| Scheduler | Worker management, global state, initialization |
| GlobalQueue | Shared queue for new bthreads |
| WorkStealingQueue | Local task queue with steal support |
| Butex | bthread mutex for cross-thread wait/wake |
| TimerThread | Timer management |
| ExecutionQueue | Ordered execution queue |
| Platform | Platform abstraction (futex, stack, context switch) |


## Core Data Structures

### TaskMeta

```cpp
struct TaskMeta {
    // ========== Stack Management ==========
    void* stack;
    size_t stack_size;        // Default: 1MB

    // ========== Context (platform-dependent) ==========
    Context context;

    // ========== State ==========
    enum State : uint8_t {
        STATE_READY,          // Ready to be scheduled
        STATE_RUNNING,        // Currently running
        STATE_SUSPENDED,      // Suspended (waiting for lock, etc.)
        STATE_FINISHED        // Finished
    };
    std::atomic<State> state;

    // ========== Entry Function and Result ==========
    void* (*fn)(void*);
    void* arg;
    void* result;             // Return value from bthread_exit

    // ========== Reference Counting ==========
    // Initial value: 1 (creator) + 1 (if joinable)
    // Incremented when: referenced by bthread_t lookup
    // Decremented when: bthread exits, join/detach completes
    // Recycled when: ref_count reaches 0
    std::atomic<int> ref_count;

    // ========== bthread_t Identification ==========
    uint32_t slot_index;      // Index in TaskGroup pool
    uint32_t generation;      // Generation counter (prevents ABA)

    // ========== Join Support ==========
    Butex* join_butex;        // Butex for join waiters
    std::atomic<int> join_waiters;

    // ========== Butex Wait State ==========
    // When a bthread waits on a butex, these fields track the wait
    Butex* waiting_butex;     // Which butex we're waiting on (nullptr if not waiting)
    WaiterState waiter;       // Embedded waiter state (lives in TaskMeta, not stack!)
    // After wait completes, waiter is reset

    // ========== Scheduling ==========
    Worker* local_worker;     // Last worker that ran this bthread
};
```

### WaiterState (Embedded in TaskMeta)

```cpp
// Waiter state stored in TaskMeta, NOT on stack
struct WaiterState {
    std::atomic<WaiterState*> next;  // Next in wait queue
    std::atomic<bool> wakeup;        // Has been woken?
    std::atomic<bool> timed_out;     // Did timeout occur?
    int64_t deadline_us;             // Absolute timeout time (0 = no timeout)
    int timer_id;                    // Timer ID for cancellation
};
```

### TaskGroup (TaskMeta Pool)

Manages TaskMeta allocation and bthread_t to TaskMeta mapping:

```cpp
class TaskGroup {
    // TaskMeta pool for reuse
    static constexpr size_t POOL_SIZE = 16384;
    std::array<std::atomic<TaskMeta*>, POOL_SIZE> task_pool_;

    // Free list: stored as linked list using slot indices
    // free_slots_[i] points to next free slot, -1 terminates
    std::array<std::atomic<int32_t>, POOL_SIZE> free_slots_;
    std::atomic<int32_t> free_head_;  // Head of free list, -1 if empty

    // Generation counters (per slot, for bthread_t encoding)
    std::array<std::atomic<uint32_t>, POOL_SIZE> generations_;

public:
    // Allocate TaskMeta from pool
    // Returns nullptr if pool exhausted
    TaskMeta* AllocTaskMeta();

    // Recycle TaskMeta back to pool
    void DeallocTaskMeta(TaskMeta* task);

    // Encode bthread_t from slot index and generation
    static constexpr bthread_t EncodeId(uint32_t slot, uint32_t gen) {
        return (static_cast<uint64_t>(gen) << 32) | slot;
    }

    // Decode bthread_t to TaskMeta (with generation check)
    TaskMeta* DecodeId(bthread_t tid) {
        uint32_t slot = static_cast<uint32_t>(tid & 0xFFFFFFFF);
        uint32_t gen = static_cast<uint32_t>(tid >> 32);
        if (slot >= POOL_SIZE) return nullptr;
        TaskMeta* meta = task_pool_[slot].load(std::memory_order_acquire);
        if (meta && meta->generation == gen) {
            return meta;
        }
        return nullptr;  // Stale bthread_t
    }

    int32_t worker_count() const {
        return worker_count_.load(std::memory_order_acquire);
    }
};
```

### TaskMeta Allocation Flow

```cpp
TaskMeta* TaskGroup::AllocTaskMeta() {
    // Try free list first
    int32_t slot = free_head_.load(std::memory_order_acquire);
    while (slot >= 0) {
        int32_t next = free_slots_[slot].load(std::memory_order_relaxed);
        if (free_head_.compare_exchange_weak(slot, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Got a slot from free list
            TaskMeta* meta = task_pool_[slot].load(std::memory_order_relaxed);
            meta->slot_index = slot;
            meta->generation = generations_[slot].load(std::memory_order_relaxed);
            return meta;
        }
    }

    // Free list empty, allocate new
    // ... (rarely needed if pool is sized correctly)
    return nullptr;
}

void TaskGroup::DeallocTaskMeta(TaskMeta* task) {
    uint32_t slot = task->slot_index;

    // Increment generation for next use
    generations_[slot].fetch_add(1, std::memory_order_relaxed);
    task->generation = generations_[slot].load(std::memory_order_relaxed);

    // Reset state
    task->state.store(STATE_READY, std::memory_order_relaxed);
    task->ref_count.store(0, std::memory_order_relaxed);
    // Note: stack is kept and reused

    // Add to free list
    int32_t old_head = free_head_.load(std::memory_order_relaxed);
    do {
        free_slots_[slot].store(old_head, std::memory_order_relaxed);
    } while (!free_head_.compare_exchange_weak(old_head, slot,
            std::memory_order_release, std::memory_order_relaxed));
}
```

### WorkStealingQueue

Lock-free double-ended queue with ABA prevention:

```cpp
class WorkStealingQueue {
    // Array-based circular buffer for better cache locality
    static constexpr size_t CAPACITY = 1024;

    std::atomic<TaskMeta*> buffer_[CAPACITY];
    std::atomic<uint64_t> head_;  // [version:32 | index:32]
    std::atomic<uint64_t> tail_;  // [version:32 | index:32]

public:
    void Push(TaskMeta* task);
    TaskMeta* Pop();
    TaskMeta* Steal();
    bool Empty() const;

private:
    static uint32_t ExtractIndex(uint64_t v) { return v & 0xFFFFFFFF; }
    static uint32_t ExtractVersion(uint64_t v) { return v >> 32; }
    static uint64_t MakeVal(uint32_t ver, uint32_t idx) {
        return (static_cast<uint64_t>(ver) << 32) | idx;
    }
};

void WorkStealingQueue::Push(TaskMeta* task) {
    uint64_t t = tail_.load(std::memory_order_relaxed);
    uint32_t idx = ExtractIndex(t);
    buffer_[idx].store(task, std::memory_order_relaxed);
    tail_.store(MakeVal(ExtractVersion(t) + 1, (idx + 1) % CAPACITY),
                std::memory_order_release);
}

TaskMeta* WorkStealingQueue::Pop() {
    uint64_t h = head_.load(std::memory_order_relaxed);
    uint64_t t = tail_.load(std::memory_order_acquire);
    if (ExtractIndex(h) == ExtractIndex(t)) return nullptr;  // Empty

    uint32_t idx = ExtractIndex(h);
    TaskMeta* task = buffer_[idx].load(std::memory_order_relaxed);
    head_.store(MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
                std::memory_order_release);
    return task;
}

TaskMeta* WorkStealingQueue::Steal() {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);
    if (ExtractIndex(h) == ExtractIndex(t)) return nullptr;

    uint32_t idx = ExtractIndex(h);
    TaskMeta* task = buffer_[idx].load(std::memory_order_acquire);

    // Verify head hasn't changed (ABA check via version)
    if (head_.load(std::memory_order_acquire) != h) {
        return nullptr;  // Concurrent modification, retry
    }

    // Try to claim
    if (head_.compare_exchange_strong(h,
            MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return task;
    }
    return nullptr;
}
```

### GlobalQueue

MPSC queue with version counter for ABA prevention:

```cpp
class GlobalQueue {
    std::atomic<TaskMeta*> head_;
    std::atomic<uint64_t> version_;  // Incremented on each modification

public:
    void Push(TaskMeta* task) {
        task->next = nullptr;  // TaskMeta has 'next' for this purpose
        TaskMeta* old_head = head_.load(std::memory_order_relaxed);
        do {
            task->next = old_head;
        } while (!head_.compare_exchange_weak(old_head, task,
                std::memory_order_release, std::memory_order_relaxed));
        version_.fetch_add(1, std::memory_order_release);
    }

    TaskMeta* Pop() {
        // Simplified: take entire list, reverse it, process
        TaskMeta* head = head_.exchange(nullptr, std::memory_order_acq_rel);
        if (!head) return nullptr;

        // Reverse list for FIFO order
        TaskMeta* result = nullptr;
        TaskMeta* next = nullptr;
        while (head) {
            next = head->next;
            head->next = result;
            result = head;
            head = next;
        }
        version_.fetch_add(1, std::memory_order_release);
        return result;
    }
};
```

### Worker

```cpp
class Worker {
    int id_;
    Platform::ThreadId thread_;
    WorkStealingQueue local_queue_;
    TaskMeta* current_task_;
    Context saved_context_;    // Context to return to after bthread suspends

    // Sleep state
    std::atomic<bool> sleeping_{false};
    std::atomic<int> sleep_token_{0};  // For butex wait

    static thread_local Worker* current_worker_;

public:
    void Run();
    TaskMeta* PickTask();
    void SuspendCurrent();
    void Resume(TaskMeta* task);

    void WaitForTask();
    void WakeUp();

    static Worker* Current() { return current_worker_; }
};
```

### Scheduler

```cpp
class Scheduler {
    std::vector<Worker*> workers_;
    std::atomic<int32_t> worker_count_{0};
    int32_t configured_count_{0};  // Set before initialization

    GlobalQueue global_queue_;

    std::unique_ptr<TimerThread> timer_thread_;
    std::once_flag timer_init_flag_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{true};
    std::once_flag init_once_;

public:
    static Scheduler& Instance() {
        static Scheduler instance;
        return instance;
    }

    void Init() {
        std::call_once(init_once_, [this]{
            int n = configured_count_;
            if (n <= 0) {
                n = std::thread::hardware_concurrency();
                if (n == 0) n = 4;
            }
            StartWorkers(n);
            initialized_.store(true, std::memory_order_release);
        });
    }

    void Shutdown() {
        running_.store(false, std::memory_order_release);
        WakeAllWorkers();
        for (auto* w : workers_) {
            Platform::JoinThread(w->thread_);
            delete w;
        }
        workers_.clear();
    }

    void StartWorkers(int count) {
        workers_.reserve(count);
        for (int i = 0; i < count; ++i) {
            Worker* w = new Worker(i);
            w->thread_ = Platform::CreateThread([](void* arg) {
                static_cast<Worker*>(arg)->Run();
            }, w);
            workers_.push_back(w);
        }
        worker_count_.store(count, std::memory_order_release);
    }

    int32_t worker_count() const {
        return worker_count_.load(std::memory_order_acquire);
    }

    TimerThread* GetTimerThread() {
        std::call_once(timer_init_flag_, [this]{
            timer_thread_ = std::make_unique<TimerThread>();
            timer_thread_->Start();
        });
        return timer_thread_.get();
    }

    void EnqueueTask(TaskMeta* task);
    void WakeIdleWorkers(int count);
};
```

## Scheduling

### Worker Main Loop

```cpp
void Worker::Run() {
    current_worker_ = this;

    while (Scheduler::Instance().running()) {
        TaskMeta* task = PickTask();
        if (task == nullptr) {
            WaitForTask();
            continue;
        }

        current_task_ = task;
        task->state.store(STATE_RUNNING, std::memory_order_release);

        // Switch to bthread
        Platform::SwapContext(&saved_context_, &task->context);

        // Returned from bthread
        current_task_ = nullptr;

        // Handle task based on its new state
        HandleTaskAfterRun(task);
    }
}

void Worker::HandleTaskAfterRun(TaskMeta* task) {
    State state = task->state.load(std::memory_order_acquire);

    switch (state) {
        case STATE_FINISHED:
            // bthread exited normally
            HandleFinishedTask(task);
            break;

        case STATE_SUSPENDED:
            // bthread suspended (waiting on butex)
            // Nothing to do - it will be re-queued when woken
            break;

        default:
            // Should not happen
            break;
    }
}

void Worker::HandleFinishedTask(TaskMeta* task) {
    // Wake up any joiners
    if (task->join_waiters.load(std::memory_order_acquire) > 0) {
        task->join_butex->Wake(INT_MAX);
    }

    // Release reference
    task->Release();
}
```

### WaitForTask

```cpp
void Worker::WaitForTask() {
    sleeping_.store(true, std::memory_order_release);

    // Double-check for tasks
    if (!local_queue_.Empty() ||
        !Scheduler::Instance().global_queue_.Empty()) {
        sleeping_.store(false, std::memory_order_relaxed);
        return;
    }

    // Sleep using platform futex
    int expected = sleep_token_.load(std::memory_order_acquire);
    Platform::FutexWait(&sleep_token_, expected, nullptr);

    sleeping_.store(false, std::memory_order_relaxed);
}

void Worker::WakeUp() {
    if (sleeping_.load(std::memory_order_acquire)) {
        sleep_token_.fetch_add(1, std::memory_order_release);
        Platform::FutexWake(&sleep_token_, 1);
    }
}
```

### Task Selection

```cpp
TaskMeta* Worker::PickTask() {
    TaskMeta* task;

    // 1. Local queue
    task = local_queue_.Pop();
    if (task) return task;

    // 2. Global queue
    task = Scheduler::Instance().global_queue_.Pop();
    if (task) return task;

    // 3. Random steal
    int32_t wc = Scheduler::Instance().worker_count();
    int attempts = wc * 3;

    for (int i = 0; i < attempts; ++i) {
        int victim = (id_ + rand()) % wc;
        if (victim == id_) continue;

        Worker* other = Scheduler::Instance().workers_[victim];
        if (other) {
            task = other->local_queue_.Steal();
            if (task) return task;
        }
    }

    return nullptr;
}
```

### Handling bthread from pthread

```cpp
int bthread_yield() {
    Worker* w = Worker::Current();
    if (w == nullptr) {
        // Called from pthread
        std::this_thread::yield();
        return 0;
    }
    // Called from bthread - switch back to scheduler
    return w->YieldCurrent();
}

int Worker::YieldCurrent() {
    if (!current_task_) return EINVAL;
    current_task_->state.store(STATE_READY, std::memory_order_release);
    local_queue_.Push(current_task_);
    SuspendCurrent();
    return 0;
}

void Worker::SuspendCurrent() {
    Platform::SwapContext(&current_task_->context, &saved_context_);
}
```

## Butex Synchronization

### Butex Structure

```cpp
class Butex {
    // Waiter queue head (points to TaskMeta with wait state)
    std::atomic<TaskMeta*> waiters_;

    // Value for conditional wait
    std::atomic<int> value_{0};

public:
    Butex() : waiters_(nullptr), value_(0) {}

    int Wait(int expected_value, const timespec* timeout);
    void Wake(int count);

private:
    static void TimeoutCallback(void* arg);
};
```

### Wait Flow (Waiter in TaskMeta)

```cpp
int Butex::Wait(int expected_value, const timespec* timeout) {
    Worker* w = Worker::Current();
    if (w == nullptr) {
        // Called from pthread, use futex directly
        return Platform::FutexWait(&value_, expected_value, timeout);
    }

    TaskMeta* task = w->current_task();

    // 1. Check value first
    if (value_.load(std::memory_order_acquire) != expected_value) {
        return 0;
    }

    // 2. Initialize wait state in TaskMeta (NOT on stack!)
    WaiterState& ws = task->waiter;
    ws.wakeup.store(false, std::memory_order_relaxed);
    ws.timed_out.store(false, std::memory_order_relaxed);
    ws.deadline_us = 0;
    ws.timer_id = 0;

    // 3. Add to wait queue (push to head)
    TaskMeta* old_head = waiters_.load(std::memory_order_relaxed);
    do {
        ws.next.store(old_head, std::memory_order_relaxed);
    } while (!waiters_.compare_exchange_weak(old_head, task,
            std::memory_order_release, std::memory_order_relaxed));

    // 4. Double-check value
    if (value_.load(std::memory_order_acquire) != expected_value) {
        // Remove from queue
        RemoveFromWaitQueue(task);
        return 0;
    }

    // 5. Set up timeout
    if (timeout) {
        ws.deadline_us = GetTimeOfDay() + ToMicroseconds(timeout);
        ws.timer_id = Scheduler::Instance().GetTimerThread()->Schedule(
            TimeoutCallback, task, timeout);
    }

    // 6. Record which butex we're waiting on
    task->waiting_butex = this;

    // 7. Suspend
    task->state.store(STATE_SUSPENDED, std::memory_order_release);
    w->SuspendCurrent();

    // 8. Resumed - check result
    task->waiting_butex = nullptr;

    if (ws.timed_out.load(std::memory_order_acquire)) {
        return ETIMEDOUT;
    }
    return 0;
}
```

### Wake Flow

```cpp
void Butex::Wake(int count) {
    int woken = 0;

    while (woken < count) {
        // Pop from head
        TaskMeta* waiter = waiters_.load(std::memory_order_acquire);
        if (!waiter) break;

        // Try to atomically claim this waiter
        TaskMeta* next = waiter->waiter.next.load(std::memory_order_relaxed);
        if (waiters_.compare_exchange_weak(waiter, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {

            WaiterState& ws = waiter->waiter;

            // Check if already woken/timed out
            bool expected = false;
            if (ws.wakeup.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                ++woken;

                // Cancel pending timeout
                if (ws.timer_id != 0) {
                    Scheduler::Instance().GetTimerThread()->Cancel(ws.timer_id);
                }

                // Re-queue the task
                waiter->state.store(STATE_READY, std::memory_order_release);
                Scheduler::Instance().EnqueueTask(waiter);
            }
        }
    }
}
```

### Timeout Callback

```cpp
void Butex::TimeoutCallback(void* arg) {
    TaskMeta* task = static_cast<TaskMeta*>(arg);
    WaiterState& ws = task->waiter;

    // Try to mark as timed out
    bool expected = false;
    if (ws.wakeup.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        ws.timed_out.store(true, std::memory_order_release);

        // Re-queue the task
        task->state.store(STATE_READY, std::memory_order_release);
        Scheduler::Instance().EnqueueTask(task);
    }
}
```

### High-Level Synchronization Primitives

```cpp
// Mutex
struct bthread_mutex_t {
    Butex butex;
    std::atomic<uint64_t> owner{0};  // bthread_t or 0

    // For pthread compatibility
    pthread_mutex_t pthread_mutex;

    bthread_mutex_t() {
        pthread_mutex_init(&pthread_mutex, nullptr);
    }
    ~bthread_mutex_t() {
        pthread_mutex_destroy(&pthread_mutex);
    }
};

// Condition Variable
struct bthread_cond_t {
    Butex butex;
    std::atomic<int> wait_count{0};
    pthread_cond_t pthread_cond;

    bthread_cond_t() {
        pthread_cond_init(&pthread_cond, nullptr);
    }
};

// One-time initialization
struct bthread_once_t {
    std::atomic<int> state{0};  // 0=not called, 1=in progress, 2=done
    Butex butex;
};
```

## bthread_create / join / detach / exit

### bthread_create

```cpp
int bthread_create(bthread_t* tid, const bthread_attr_t* attr,
                   void* (*fn)(void*), void* arg) {
    Scheduler::Instance().Init();  // Lazy init

    // Allocate TaskMeta
    TaskMeta* task = TaskGroup::Instance().AllocTaskMeta();
    if (!task) return EAGAIN;

    // Set up stack
    size_t stack_size = attr ? attr->stack_size : BTHREAD_STACK_SIZE_DEFAULT;
    if (!task->stack) {
        task->stack = Platform::AllocateStack(stack_size);
        task->stack_size = stack_size;
    }

    // Initialize
    task->fn = fn;
    task->arg = arg;
    task->result = nullptr;
    task->state.store(STATE_READY, std::memory_order_relaxed);
    task->ref_count.store(2, std::memory_order_relaxed);  // Creator + joinable
    task->join_waiters.store(0, std::memory_order_relaxed);
    task->join_butex = new Butex();

    // Set up context
    Platform::MakeContext(&task->context, task->stack, task->stack_size,
                          BthreadEntry, task);

    // Encode bthread_t
    *tid = TaskGroup::Instance().EncodeId(task->slot_index, task->generation);

    // Enqueue
    Scheduler::Instance().EnqueueTask(task);

    return 0;
}

static void BthreadEntry(void* arg) {
    TaskMeta* task = static_cast<TaskMeta*>(arg);
    task->result = task->fn(task->arg);
    bthread_exit(task->result);
}
```

### bthread_join

```cpp
int bthread_join(bthread_t tid, void** retval) {
    TaskMeta* task = TaskGroup::Instance().DecodeId(tid);
    if (!task) return ESRCH;

    // Check if trying to join self
    Worker* w = Worker::Current();
    if (w && w->current_task() == task) {
        return EDEADLK;
    }

    // Check if already finished
    if (task->state.load(std::memory_order_acquire) == STATE_FINISHED) {
        if (retval) *retval = task->result;
        task->Release();
        return 0;
    }

    // Wait for completion
    task->join_waiters.fetch_add(1, std::memory_order_acq_rel);
    task->join_butex->Wait(0, nullptr);
    task->join_waiters.fetch_sub(1, std::memory_order_acq_rel);

    if (retval) *retval = task->result;
    task->Release();
    return 0;
}
```

### bthread_detach

```cpp
int bthread_detach(bthread_t tid) {
    TaskMeta* task = TaskGroup::Instance().DecodeId(tid);
    if (!task) return ESRCH;

    // Decrement ref count (was 2 for joinable, now 1)
    if (task->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Already finished, recycle immediately
        TaskGroup::Instance().DeallocTaskMeta(task);
    }

    return 0;
}
```

### bthread_exit

```cpp
void bthread_exit(void* retval) {
    Worker* w = Worker::Current();
    if (!w || !w->current_task()) {
        // Called from pthread - just return
        return;
    }

    TaskMeta* task = w->current_task();
    task->result = retval;
    task->state.store(STATE_FINISHED, std::memory_order_release);

    // Decrement ref count
    task->Release();

    // Switch back to scheduler (never returns)
    Platform::SwapContext(&task->context, &w->saved_context_);
}
```

### bthread_self

```cpp
bthread_t bthread_self() {
    Worker* w = Worker::Current();
    if (!w || !w->current_task()) {
        return 0;  // Not in a bthread
    }
    TaskMeta* task = w->current_task();
    return TaskGroup::Instance().EncodeId(task->slot_index, task->generation);
}
```

## Timer Thread

```cpp
class TimerThread {
    struct TimerTask {
        int64_t deadline_us;
        void (*callback)(void*);
        void* arg;
        int id;
        std::atomic<bool> cancelled{false};
    };

    // Store pointers, not objects, to avoid atomic issues
    std::vector<TimerTask*> tasks_;
    std::mutex mutex_;
    std::atomic<int> wait_token_{0};
    std::atomic<bool> running_{false};
    Platform::ThreadId thread_;
    std::atomic<int> next_id_{1};

public:
    void Start() {
        running_.store(true, std::memory_order_release);
        thread_ = Platform::CreateThread([](void* arg) {
            static_cast<TimerThread*>(arg)->Run();
        }, this);
    }

    int Schedule(void (*callback)(void*), void* arg, const timespec* delay) {
        TimerTask* task = new TimerTask;
        task->deadline_us = GetTimeOfDay() + ToMicroseconds(delay);
        task->callback = callback;
        task->arg = arg;
        task->id = next_id_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(task);
            std::push_heap(tasks_.begin(), tasks_.end(),
                [](TimerTask* a, TimerTask* b) {
                    return a->deadline_us > b->deadline_us;
                });
        }

        // Wake timer thread
        wait_token_.fetch_add(1, std::memory_order_release);
        Platform::FutexWake(&wait_token_, 1);

        return task->id;
    }

    bool Cancel(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* task : tasks_) {
            if (task->id == id) {
                task->cancelled.store(true, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    void Run() {
        while (running_.load(std::memory_order_acquire)) {
            int64_t now = GetTimeOfDay();
            int64_t next_deadline = INT64_MAX;

            {
                std::lock_guard<std::mutex> lock(mutex_);

                while (!tasks_.empty() && tasks_.front()->deadline_us <= now) {
                    TimerTask* task = tasks_.front();
                    std::pop_heap(tasks_.begin(), tasks_.end(),
                        [](TimerTask* a, TimerTask* b) {
                            return a->deadline_us > b->deadline_us;
                        });
                    tasks_.pop_back();

                    if (!task->cancelled.load(std::memory_order_acquire)) {
                        task->callback(task->arg);
                    }
                    delete task;
                }

                if (!tasks_.empty()) {
                    next_deadline = tasks_.front()->deadline_us;
                }
            }

            // Wait
            int64_t wait_us = next_deadline - GetTimeOfDay();
            if (wait_us > 0) {
                timespec ts = FromMicroseconds(wait_us);
                int expected = wait_token_.load(std::memory_order_acquire);
                Platform::FutexWait(&wait_token_, expected, &ts);
            }
        }
    }
};
```

## ExecutionQueue

```cpp
class ExecutionQueue {
    struct Task {
        void (*execute)(void*, void*);
        void* arg;
        void** result_ptr;
        std::atomic<bool> done{false};
        int done_token{0};  // For futex wait
        Task* next;
    };

    std::atomic<Task*> head_{nullptr};
    std::atomic<Task*> tail_{nullptr};
    std::atomic<bool> stopped_{false};

public:
    int Execute(void (*fn)(void*, void*), void* arg, void** result) {
        Task* task = new Task;
        task->execute = fn;
        task->arg = arg;
        task->result_ptr = result;
        task->done.store(false, std::memory_order_relaxed);

        // Push
        Task* old_tail = tail_.load(std::memory_order_relaxed);
        do {
            task->next = old_tail;
        } while (!tail_.compare_exchange_weak(old_tail, task,
                std::memory_order_release, std::memory_order_relaxed));

        if (result) {
            // Wait for completion
            int expected = task->done_token;
            while (!task->done.load(std::memory_order_acquire)) {
                Platform::FutexWait(&task->done_token, expected, nullptr);
            }
            *result = *task->result_ptr;
            delete task;
        }

        return 0;
    }

    // Called by executor thread
    void ExecuteOne() {
        Task* task = head_.load(std::memory_order_acquire);
        if (!task) return;

        // Pop from head
        Task* next = task->next;
        if (head_.compare_exchange_strong(task, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            task->execute(task->arg, task->result_ptr);
            task->done.store(true, std::memory_order_release);
            task->done_token++;
            Platform::FutexWake(&task->done_token, 1);
        }
    }
};
```

## Platform Abstraction

### Context Structure (Sufficient for All ABIs)

```cpp
struct Context {
    // x86-64 System V: rbx, rbp, r12-r15 (6 registers)
    // Windows x64: rbx, rbp, rsi, rdi, r12-r15, xmm6-xmm15 (10 GPR + 10 XMM)
    // ARM64: x19-x28, d8-d15 (10 GPR + 8 FP)

    union {
        uint64_t gp_regs[16];     // General purpose registers
        void* ptr_regs[16];
    };

    // XMM registers for Windows (10 x 16 bytes = 160 bytes)
    alignas(16) uint8_t xmm_regs[160];

    // Stack pointer
    void* stack_ptr;

    // Return address (for wrapper)
    void* return_addr;
};
```

### Linux Context Implementation (Assembly)

```cpp
// x86-64 Linux assembly for context switching
// In platform_linux_x64.S:

// void SwapContext(Context* from, Context* to)
// rd = rdi, to = rsi
SwapContext:
    // Save callee-saved registers
    mov     [rdi + 0*8], rbx
    mov     [rdi + 1*8], rbp
    mov     [rdi + 2*8], r12
    mov     [rdi + 3*8], r13
    mov     [rdi + 4*8], r14
    mov     [rdi + 5*8], r15
    mov     [rdi + 14*8], rsp     // stack_ptr offset

    // Load from 'to'
    mov     rbx, [rsi + 0*8]
    mov     rbp, [rsi + 1*8]
    mov     r12, [rsi + 2*8]
    mov     r13, [rsi + 3*8]
    mov     r14, [rsi + 4*8]
    mov     r15, [rsi + 5*8]
    mov     rsp, [rsi + 14*8]

    ret
```

### Windows Context Implementation

```cpp
// In platform_windows_x64.asm:
// Similar structure but saves xmm6-xmm15

void SwapContext(Context* from, Context* to) {
    // Save GPRs
    from->gp_regs[0] = rbx;
    // ... (in assembly)

    // Save XMM registers
    _mm_store_si128((__m128i*)&from->xmm_regs[0], xmm6);
    _mm_store_si128((__m128i*)&from->xmm_regs[16], xmm7);
    // ... through xmm15

    // Load to context
    // ...
}
```

### Stack Allocation with Overflow Detection

```cpp
void* Platform::AllocateStack(size_t size) {
    size_t total = size + PAGE_SIZE;

#ifdef __linux__
    void* ptr = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    // Guard page at lowest address
    mprotect(ptr, PAGE_SIZE, PROT_NONE);

    // Set up signal handler for SIGSEGV to detect stack overflow
    // (Done once at initialization)

#else  // Windows
    void* ptr = VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    if (!ptr) return nullptr;

    DWORD old;
    VirtualProtect(ptr, PAGE_SIZE, PAGE_NOACCESS, &old);

    // Set up SEH handler for stack overflow
#endif

    // Stack top is at highest address, 16-byte aligned
    void* stack_top = (char*)ptr + total;
    stack_top = (void*)((uintptr_t)stack_top & ~0xF);
    return stack_top;
}
```

### Stack Overflow Handling

```cpp
// Linux: SIGSEGV handler
void StackOverflowHandler(int sig, siginfo_t* info, void* ctx) {
    // Check if fault is in a guard page
    // If so, report stack overflow and abort
    // Otherwise, chain to previous handler
}

// Windows: SEH handler
LONG WINAPI StackOverflowHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        // Report and abort
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Called during Scheduler::Init
void SetupStackOverflowHandler() {
#ifdef __linux__
    struct sigaction sa;
    sa.sa_sigaction = StackOverflowHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
#else
    SetUnhandledExceptionFilter(StackOverflowHandler);
#endif
}
```

### Futex Implementation

```cpp
// Linux
int Platform::FutexWait(std::atomic<int>* addr, int expected,
                        const timespec* timeout) {
    int ret = syscall(SYS_futex, (int*)addr, FUTEX_WAIT_PRIVATE,
                      expected, timeout, nullptr, 0);
    if (ret == -1) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        if (errno == ETIMEDOUT) return ETIMEDOUT;
        return errno;
    }
    return 0;
}

int Platform::FutexWake(std::atomic<int>* addr, int count) {
    syscall(SYS_futex, (int*)addr, FUTEX_WAKE_PRIVATE, count,
            nullptr, nullptr, 0);
    return 0;
}

// Windows
int Platform::FutexWait(std::atomic<int>* addr, int expected,
                        const timespec* timeout) {
    DWORD ms = timeout ? ToMilliseconds(timeout) : INFINITE;
    BOOL ok = WaitOnAddress(static_cast<volatile VOID*>(addr),
                            &expected, sizeof(int), ms);
    if (!ok) {
        DWORD err = GetLastError();
        return (err == ERROR_TIMEOUT) ? ETIMEDOUT : 0;
    }
    return 0;
}

int Platform::FutexWake(std::atomic<int>* addr, int count) {
    if (count == 1) {
        WakeByAddressSingle(static_cast<volatile VOID*>(addr));
    } else {
        WakeByAddressAll(static_cast<volatile VOID*>(addr));
    }
    return 0;
}
```

## Public API

```cpp
#ifdef __cplusplus
extern "C" {
#endif

// ========== Basic bthread API ==========
typedef uint64_t bthread_t;

int bthread_create(bthread_t* tid, const bthread_attr_t* attr,
                   void* (*fn)(void*), void* arg);
int bthread_join(bthread_t tid, void** retval);
int bthread_detach(bthread_t tid);
bthread_t bthread_self();
int bthread_yield();
void bthread_exit(void* retval);

// ========== Attributes ==========
#define BTHREAD_STACK_SIZE_DEFAULT (1024 * 1024)

typedef struct {
    size_t stack_size;
    const char* name;    // For debugging/logging (optional)
} bthread_attr_t;

#define BTHREAD_ATTR_INIT { BTHREAD_STACK_SIZE_DEFAULT, nullptr }

static inline int bthread_attr_init(bthread_attr_t* attr) {
    attr->stack_size = BTHREAD_STACK_SIZE_DEFAULT;
    attr->name = nullptr;
    return 0;
}

static inline int bthread_attr_destroy(bthread_attr_t* attr) {
    return 0;
}

// ========== Synchronization Primitives ==========
typedef struct bthread_mutex_t bthread_mutex_t;
typedef struct bthread_cond_t bthread_cond_t;
typedef struct bthread_once_t bthread_once_t;

#define BTHREAD_MUTEX_INIT { .butex = Butex(), .owner = 0 }
#define BTHREAD_COND_INIT { .butex = Butex(), .wait_count = 0 }
#define BTHREAD_ONCE_INIT { .state = 0, .butex = Butex() }

int bthread_mutex_init(bthread_mutex_t* mutex, const void* attr);
int bthread_mutex_destroy(bthread_mutex_t* mutex);
int bthread_mutex_lock(bthread_mutex_t* mutex);
int bthread_mutex_unlock(bthread_mutex_t* mutex);
int bthread_mutex_trylock(bthread_mutex_t* mutex);

int bthread_cond_init(bthread_cond_t* cond, const void* attr);
int bthread_cond_destroy(bthread_cond_t* cond);
int bthread_cond_wait(bthread_cond_t* cond, bthread_mutex_t* mutex);
int bthread_cond_timedwait(bthread_cond_t* cond, bthread_mutex_t* mutex,
                           const struct timespec* abstime);
int bthread_cond_signal(bthread_cond_t* cond);
int bthread_cond_broadcast(bthread_cond_t* cond);

int bthread_once(bthread_once_t* once, void (*init)());

// ========== Timer ==========
typedef int bthread_timer_t;

bthread_timer_t bthread_timer_add(void (*callback)(void*), void* arg,
                                   const struct timespec* delay);
int bthread_timer_cancel(bthread_timer_t timer_id);

// ========== Global Configuration ==========
int bthread_set_worker_count(int count);
int bthread_get_worker_count();

#ifdef __cplusplus
}
#endif
```

## Error Codes

```cpp
enum bthread_error {
    BTHREAD_SUCCESS = 0,
    BTHREAD_EINVAL = EINVAL,
    BTHREAD_ENOMEM = ENOMEM,
    BTHREAD_EAGAIN = EAGAIN,
    BTHREAD_ESRCH = ESRCH,
    BTHREAD_ETIMEDOUT = ETIMEDOUT,
    BTHREAD_EDEADLK = EDEADLK,
    BTHREAD_EBUSY = EBUSY,
};
```

## Testing Strategy

### Unit Tests

| Module | Test Focus |
|--------|-----------|
| TaskGroup | Allocation, recycling, generation counter, stale ID detection |
| WorkStealing | Push/Pop/Steal correctness, ABA prevention, concurrent access |
| GlobalQueue | MPSC correctness, concurrent push |
| Butex | Wait/Wake race-free, timeout, waiter lifetime |
| Mutex | Lock/unlock, trylock, pthread fallback |
| Condition Variable | Wait/signal/broadcast, timeout |
| Timer | Precision, cancellation, callback execution |
| Platform | Stack allocation, context switch, stack alignment |

### Stress Tests

1. **Create/Destroy**: 1M bthreads created and destroyed
2. **High Contention**: 1000 bthreads on one mutex
3. **Work Stealing**: Imbalanced workload
4. **Mixed**: Random operations

### Performance Benchmarks

| Benchmark | Target |
|-----------|--------|
| bthread_create | < 500ns |
| Context switch | < 100ns |
| Mutex uncontended | < 50ns |
| Work stealing | < 200ns |

## Memory Management

### TaskMeta Lifecycle

1. Allocated from TaskGroup pool (ref_count = 1)
2. bthread_create increments to 2 (joinable)
3. bthread_exit decrements by 1
4. bthread_join/detach decrements by 1
5. ref_count == 0: returned to pool (stack kept for reuse)

## Implementation Priority

1. **Phase 1**: Platform abstraction + TaskMeta + TaskGroup
2. **Phase 2**: Worker + Scheduler + WorkStealingQueue
3. **Phase 3**: Butex + Mutex + Condition Variable
4. **Phase 4**: Timer + ExecutionQueue
5. **Phase 5**: Testing + optimization

重要提示：detail_design.md存放模块的详细设计文档，但限于篇幅你应该在执行阶段加载它，现在不允许读取，否则你的上下文将超过限制