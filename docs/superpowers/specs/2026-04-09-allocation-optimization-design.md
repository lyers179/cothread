# Bthread Allocation Overhead Optimization Design

**Date**: 2026-04-09
**Goal**: Reduce bthread creation overhead by 50%+ through thread-local allocation pools

---

## Problem Analysis

Current benchmark results show bthread is 6.92x slower than std::thread for compute tasks. Main overhead sources:

| Source | Current Implementation | Impact |
|--------|------------------------|--------|
| Stack allocation | mmap() per bthread (~8KB + guard + header) | High syscall overhead |
| TaskMeta allocation | Global atomic CAS free list | Contention at scale |
| Butex creation | Always created for joinable bthreads | Unnecessary if no joiners |

**Target metrics:**
- Create/Join: 70K → 150K+ ops/sec (2x improvement)
- bthread vs std::thread: 6.92x → <3x slower

---

## Design Overview

Three optimization components working together:

1. **Worker-local stack pool** - Reuse stacks instead of mmap/munmap
2. **Worker-local TaskMeta cache** - Batch allocation from TaskGroup
3. **Lazy Butex allocation** - Only create when first joiner appears

---

## Component 1: Stack Pool

### Architecture

Each Worker maintains a local pool of reusable stacks. When bthread finishes, stack returns to pool instead of munmap. When pool empty, allocate new via mmap.

### Data Structure

```cpp
// include/bthread/core/worker.hpp
class Worker {
public:
    // Stack pool configuration
    static constexpr int STACK_POOL_SIZE = 8;
    static constexpr size_t DEFAULT_STACK_SIZE = 8192;

private:
    // Stack pool - pre-allocated reusable stacks
    void* stack_pool_[STACK_POOL_SIZE];
    int stack_pool_count_{0};
    size_t default_stack_size_{DEFAULT_STACK_SIZE};

public:
    // Stack pool operations
    void* AcquireStack(size_t size = DEFAULT_STACK_SIZE);
    void ReleaseStack(void* stack_top, size_t size);
    void DrainStackPool();  // Called during shutdown
};
```

### Implementation

**AcquireStack**:
```cpp
void* Worker::AcquireStack(size_t size) {
    // 1. Try local pool first
    if (stack_pool_count_ > 0 && size <= default_stack_size_) {
        return stack_pool_[--stack_pool_count_];
    }

    // 2. Pool empty or wrong size - allocate new
    return platform::AllocateStack(size);
}
```

**ReleaseStack**:
```cpp
void* Worker::ReleaseStack(void* stack_top, size_t size) {
    // 1. Try return to local pool
    if (stack_pool_count_ < STACK_POOL_SIZE && size == default_stack_size_) {
        stack_pool_[stack_pool_count_++] = stack_top;
        return nullptr;  // Successfully pooled
    }

    // 2. Pool full or wrong size - deallocate
    platform::DeallocateStack(stack_top, size);
    return nullptr;
}
```

**Stack affinity**: When bthread finishes, stack returns to the worker that was running it (current_worker). This avoids cross-worker synchronization.

### Shutdown Handling

```cpp
void Worker::DrainStackPool() {
    for (int i = 0; i < stack_pool_count_; ++i) {
        platform::DeallocateStack(stack_pool_[i], default_stack_size_);
    }
    stack_pool_count_ = 0;
}
```

---

## Component 2: TaskMeta Cache

### Architecture

Each Worker maintains a local cache of TaskMetas. When cache empty, worker requests batch allocation from TaskGroup using a single atomic operation.

### Data Structure

```cpp
// include/bthread/core/worker.hpp
class Worker {
public:
    static constexpr int TASK_CACHE_SIZE = 4;

private:
    TaskMeta* task_cache_[TASK_CACHE_SIZE];
    int task_cache_count_{0};

public:
    TaskMeta* AcquireTaskMeta();
    void ReleaseTaskMeta(TaskMeta* meta);
    void RefillTaskCache();
    void DrainTaskCache();  // Called during shutdown
};

// include/bthread/core/task_group.hpp
class TaskGroup {
public:
    // Bulk slot allocation - reduces CAS operations
    int AllocMultipleSlots(int32_t* slots, int count);
};
```

### Implementation

**AcquireTaskMeta**:
```cpp
TaskMeta* Worker::AcquireTaskMeta() {
    // 1. Try local cache
    if (task_cache_count_ > 0) {
        return task_cache_[--task_cache_count_];
    }

    // 2. Cache empty - refill from TaskGroup
    RefillTaskCache();
    if (task_cache_count_ > 0) {
        return task_cache_[--task_cache_count_];
    }

    // 3. TaskGroup exhausted - return nullptr
    return nullptr;
}
```

**RefillTaskCache**:
```cpp
void Worker::RefillTaskCache() {
    int32_t slots[TASK_CACHE_SIZE];
    int count = GetTaskGroup().AllocMultipleSlots(slots, TASK_CACHE_SIZE);

    for (int i = 0; i < count; ++i) {
        TaskMeta* meta = GetTaskGroup().GetOrCreateTaskMeta(slots[i]);
        task_cache_[task_cache_count_++] = meta;
    }
}
```

**AllocMultipleSlots** (TaskGroup):
```cpp
int TaskGroup::AllocMultipleSlots(int32_t* slots, int count) {
    int32_t head = free_head_.load(std::memory_order_acquire);
    int allocated = 0;

    while (head >= 0 && allocated < count) {
        // Walk the free list to collect slots
        int32_t next = free_slots_[head].load(std::memory_order_relaxed);
        slots[allocated++] = head;
        head = next;
    }

    // Single CAS to update free_head
    if (allocated > 0) {
        int32_t new_head = (allocated < count) ? head : -1;
        int32_t expected = free_head_.load(std::memory_order_relaxed);
        // Note: May need retry if CAS fails due to concurrent allocation
        while (!free_head_.compare_exchange_weak(expected, new_head,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // CAS failed - retry with updated expected
            // This is a simplification; full implementation needs careful handling
        }
    }

    return allocated;
}
```

**ReleaseTaskMeta**: Returns to current worker's cache, with fallback to TaskGroup if cache full.

### Task Affinity

When TaskMeta is released, it goes to the current worker's cache. This creates task affinity - workers tend to reuse the same TaskMetas, improving cache locality.

---

## Component 3: Lazy Butex Allocation

### Architecture

Butex for join synchronization is only created when first joiner appears, not during bthread creation.

### Implementation

**bthread_create** (no change to signature):
```cpp
// Currently: join_butex = new Butex() for joinable bthreads
// Optimization: join_butex = nullptr, allocate lazily

int bthread_create(bthread_t* tid, const bthread_attr_t* attr,
                   void* (*fn)(void*), void* arg) {
    TaskMeta* meta = ...;

    // Set join_butex to nullptr - will be allocated lazily if needed
    meta->join_butex = nullptr;

    // ... rest of creation logic unchanged
}
```

**bthread_join**:
```cpp
int bthread_join(bthread_t bt, void** retval) {
    TaskMeta* task = GetTaskGroup().DecodeId(bt);
    if (!task) return ESRCH;

    // Lazy Butex allocation with atomic CAS to prevent double allocation
    if (task->join_butex == nullptr) {
        Butex* new_butex = new Butex();
        void* expected = nullptr;
        // Atomic CAS - only one joiner creates the Butex
        if (!task->join_butex.compare_exchange_strong(expected, new_butex,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Another joiner already created it
            delete new_butex;
        }
    }

    // Check if already finished
    if (task->state.load() == TaskState::FINISHED) {
        if (retval) *retval = task->result;
        return 0;
    }

    // Increment join waiters count
    task->join_waiters.fetch_add(1, std::memory_order_relaxed);

    // Wait on Butex
    Butex* butex = static_cast<Butex*>(task->join_butex);
    int expected_value = butex->value();
    butex->Wait(expected_value, nullptr, false);

    // ... cleanup and return result
}
```

**bthread finish handling**:
```cpp
void Worker::HandleFinishedBthread(TaskMeta* task) {
    // Only wake joiners if Butex was created
    if (task->join_waiters.load() > 0 && task->join_butex != nullptr) {
        Butex* butex = static_cast<Butex*>(task->join_butex);
        butex->set_value(butex->value() + 1);
        Scheduler::Instance().WakeButex(task->join_butex, INT_MAX);
    }

    // Release resources
    ReleaseStack(task->stack, task->stack_size);
    ReleaseTaskMeta(task);
}
```

---

## Thread Safety

| Component | Thread Safety Mechanism |
|-----------|------------------------|
| Stack pool | Worker-local, no synchronization needed |
| TaskMeta cache | Worker-local for acquire/release, atomic for TaskGroup batch |
| Lazy Butex | Atomic CAS prevents double allocation |

**Key invariant**: All pool operations happen in the context of a Worker thread, so no cross-worker synchronization for pool operations.

---

## Testing Strategy

### Unit Tests

**tests/stack_pool_test.cpp**:
```cpp
TEST(StackPoolTest, BasicReuse) {
    Worker w(0);
    void* s1 = w.AcquireStack();
    w.ReleaseStack(s1, DEFAULT_STACK_SIZE);
    void* s2 = w.AcquireStack();
    EXPECT_EQ(s1, s2);  // Same stack reused
}

TEST(StackPoolTest, PoolFull) {
    Worker w(0);
    void* stacks[STACK_POOL_SIZE + 1];
    for (int i = 0; i <= STACK_POOL_SIZE; ++i) {
        stacks[i] = w.AcquireStack();
    }
    // Pool full - last one should be new allocation
    EXPECT_NE(stacks[STACK_POOL_SIZE-1], stacks[STACK_POOL_SIZE]);

    // Release should deallocate when pool full
    w.ReleaseStack(stacks[STACK_POOL_SIZE], DEFAULT_STACK_SIZE);
}

TEST(StackPoolTest, ShutdownCleanup) {
    Worker w(0);
    for (int i = 0; i < STACK_POOL_SIZE; ++i) {
        w.AcquireStack();
    }
    w.DrainStackPool();
    EXPECT_EQ(w.stack_pool_count(), 0);
}
```

**tests/task_cache_test.cpp**:
```cpp
TEST(TaskCacheTest, BasicReuse) {
    Worker w(0);
    TaskMeta* t1 = w.AcquireTaskMeta();
    w.ReleaseTaskMeta(t1);
    TaskMeta* t2 = w.AcquireTaskMeta();
    EXPECT_EQ(t1, t2);  // Same TaskMeta reused
}

TEST(TaskCacheTest, CacheRefill) {
    Worker w(0);
    // Exhaust cache
    for (int i = 0; i < TASK_CACHE_SIZE + 1; ++i) {
        w.AcquireTaskMeta();
    }
    // Should have refilled from TaskGroup
}

TEST(TaskCacheTest, TaskGroupExhaustion) {
    // Create many workers to exhaust TaskGroup
    // Verify nullptr returned when truly exhausted
}
```

**tests/butex_lazy_test.cpp**:
```cpp
TEST(ButexLazyTest, NoJoinNoAlloc) {
    bthread_t tid;
    bthread_create(&tid, nullptr, empty_task, nullptr);
    bthread_join(tid, nullptr);  // Creates Butex lazily

    // Butex should only exist after join called
}

TEST(ButexLazyTest, MultipleJoiners) {
    bthread_t tid;
    bthread_create(&tid, nullptr, long_task, nullptr);

    // Two joiners - only one Butex should be created
    std::thread j1([&]() { bthread_join(tid, nullptr); });
    std::thread j2([&]() { bthread_join(tid, nullptr); });
    j1.join();
    j2.join();
}

TEST(ButexLazyTest, AlreadyFinished) {
    bthread_t tid;
    bthread_create(&tid, nullptr, empty_task, nullptr);
    usleep(1000);  // Let it finish
    bthread_join(tid, nullptr);  // Should return immediately
}
```

### Performance Tests

**benchmark/allocation_benchmark.cpp**:
```cpp
void benchmark_create_join_pool(int num_threads, int iterations) {
    // Measure create/join throughput with pools
    // Compare against baseline
}

void benchmark_stack_allocation() {
    // Measure stack acquire/release latency
    // Compare mmap vs pool
}

void benchmark_taskmeta_allocation() {
    // Measure TaskMeta acquire/release latency
    // Compare global CAS vs local cache
}
```

### Stress Tests

```cpp
TEST(StressTest, BurstWorkload) {
    // Create 10K bthreads in burst
    // Verify pools handle burst correctly
    // Verify no memory leaks
}

TEST(StressTest, LongRunning) {
    // Run for 1 minute continuous create/destroy
    // Verify pool sizes remain bounded
    // Verify no memory growth
}
```

---

## Expected Performance Impact

| Benchmark | Before | After Target | Improvement |
|-----------|--------|--------------|-------------|
| Create/Join | 70K ops/sec | 150K+ ops/sec | 2x+ |
| bthread vs std::thread | 6.92x slower | <3x slower | 2x+ |
| Stack allocation latency | ~2us (mmap) | ~50ns (pool) | 40x |
| TaskMeta allocation latency | ~200ns (CAS) | ~20ns (cache) | 10x |

---

## Risks and Mitigation

| Risk | Mitigation |
|------|------------|
| Memory growth from pools | Bounded pool size (8 stacks, 4 TaskMetas per worker) |
| Pool exhaustion under burst | Fallback to direct allocation |
| Stack affinity causing imbalance | Optional: return stack to TaskGroup if local pool full |
| Lazy Butex race condition | Atomic CAS for allocation |
| Shutdown cleanup complexity | DrainStackPool/DrainTaskCache in Worker destructor |

---

## Implementation Order

1. **Phase 1**: Stack Pool (highest impact, lowest risk)
   - Add Worker stack pool fields
   - Implement AcquireStack/ReleaseStack
   - Update bthread creation to use AcquireStack
   - Update bthread finish to use ReleaseStack
   - Add shutdown cleanup
   - Test and benchmark

2. **Phase 2**: TaskMeta Cache
   - Add Worker task cache fields
   - Implement AcquireTaskMeta/ReleaseTaskMeta/RefillTaskCache
   - Add TaskGroup AllocMultipleSlots
   - Update bthread creation/finish
   - Test and benchmark

3. **Phase 3**: Lazy Butex Allocation
   - Modify bthread_create to not allocate Butex
   - Modify bthread_join to allocate lazily
   - Update HandleFinishedBthread
   - Test and benchmark

4. **Phase 4**: Integration Testing
   - Full benchmark suite
   - Stress tests
   - Memory leak detection
   - Final performance comparison

---

## Files to Modify

| File | Changes |
|------|---------|
| `include/bthread/core/worker.hpp` | Add stack_pool_, task_cache_ fields and methods |
| `src/bthread/core/worker.cpp` | Implement pool/cache methods |
| `include/bthread/core/task_group.hpp` | Add AllocMultipleSlots method |
| `src/bthread/core/task_group.cpp` | Implement bulk slot allocation |
| `src/bthread.cpp` | Update bthread_create/bthread_join for lazy Butex |
| `tests/stack_pool_test.cpp` | New test file |
| `tests/task_cache_test.cpp` | New test file |
| `tests/butex_lazy_test.cpp` | New test file |
| `benchmark/benchmark.cpp` | Add allocation-specific benchmarks |