# Performance Phase 3 Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve bthread performance by reducing false sharing, optimizing hot paths, and adding adaptive tuning mechanisms.

**Architecture:** Three focused optimization areas: (1) Cache line alignment to eliminate false sharing between Worker fields, (2) Owner fast path optimization in WorkStealingQueue, (3) Batch wake optimization in Scheduler Submit path.

**Tech Stack:** C++20, atomic operations, cache line alignment, x86 pause instruction

---

## Current Performance Baseline

| Metric | Value |
|--------|-------|
| Create/Join | 83K ops/sec |
| vs std::thread | 3.43x faster |
| Scalability (8w) | 9.49x |
| Yield | 7.9M/sec |
| Mutex Contention | 12M/sec |

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/bthread/core/worker.hpp` | Worker class definition with cache-aligned fields |
| `src/bthread/core/worker.cpp` | Worker implementation, adaptive spin tuning |
| `include/bthread/queue/work_stealing_queue.hpp` | Queue with owner fast path hint |
| `src/bthread/queue/work_stealing_queue.cpp` | Owner-optimized Pop implementation |
| `include/bthread/core/scheduler.hpp` | Batch submit interface |
| `src/bthread/core/scheduler.cpp` | Batch wake optimization |
| `benchmark/benchmark.cpp` | Performance validation |

---

## Task 1: Cache Line Alignment for Worker Hot Fields

**Files:**
- Modify: `include/bthread/core/worker.hpp:138-145`

**Problem:** `wake_count_` and `is_idle_` are adjacent atomic fields causing false sharing when workers wake each other.

**Solution:** Use `alignas(CACHE_LINE_SIZE)` to separate them onto different cache lines.

- [ ] **Step 1: Add cache line alignment to Worker fields**

```cpp
// include/bthread/core/worker.hpp around line 138

// Stop flag - set to non-zero to stop the worker
alignas(CACHE_LINE_SIZE) std::atomic<int> stop_flag_{0};

// Wake counter - increments on each WakeUp, also increments on Stop
// Separated from is_idle_ to avoid false sharing during wake operations
alignas(CACHE_LINE_SIZE) std::atomic<int> wake_count_{0};

// Idle flag - true when worker is waiting in futex
// Separated from wake_count_ to avoid false sharing
alignas(CACHE_LINE_SIZE) std::atomic<bool> is_idle_{false};
```

- [ ] **Step 2: Add CACHE_LINE_SIZE to Worker header**

`CACHE_LINE_SIZE` is defined in `work_stealing_queue.hpp`. Add it to worker.hpp or use a shared definition:

```cpp
// include/bthread/core/worker.hpp
// Use existing CACHE_LINE_SIZE definition (64 bytes on x86)
static constexpr size_t CACHE_LINE_SIZE = 64;
```

- [ ] **Step 3: Build and run tests**

```bash
cd build && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass

- [ ] **Step 4: Run benchmark and verify no regression**

```bash
./benchmark/benchmark
```

Expected: Performance unchanged or slightly improved

- [ ] **Step 5: Commit**

```bash
git add include/bthread/core/worker.hpp
git commit -m "perf(worker): add cache line alignment to prevent false sharing

Align stop_flag_, wake_count_, and is_idle_ to separate cache lines.
This prevents false sharing during wake operations between workers.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 2: WorkStealingQueue Owner Fast Path Optimization

**Files:**
- Modify: `include/bthread/queue/work_stealing_queue.hpp:35-45`
- Modify: `src/bthread/queue/work_stealing_queue.cpp:23-51`

**Problem:** Pop() uses complex atomic logic even when called by owner. Owner could use simpler approach since it has exclusive access to the tail.

**Solution:** Add `PopOwner()` method that uses relaxed atomics for owner-only operations.

- [ ] **Step 1: Add PopOwner method declaration**

```cpp
// include/bthread/queue/work_stealing_queue.hpp

/**
 * @brief Pop from the owner's end (LIFO) - optimized for single-thread owner use.
 * Owner has exclusive access to tail, can use relaxed atomics.
 */
TaskMetaBase* PopOwner();
```

- [ ] **Step 2: Implement PopOwner with relaxed atomics**

```cpp
// src/bthread/queue/work_stealing_queue.cpp

TaskMetaBase* WorkStealingQueue::PopOwner() {
    // Owner has exclusive tail access - use relaxed ordering
    uint64_t t = tail_.load(std::memory_order_relaxed);
    uint64_t h = head_.load(std::memory_order_acquire);  // Need acquire for stealers

    if (ExtractIndex(t) == ExtractIndex(h)) {
        return nullptr;  // Empty
    }

    // LIFO: take from tail
    uint32_t idx = (ExtractIndex(t) - 1 + CAPACITY) % CAPACITY;
    TaskMetaBase* task = buffer_[idx].load(std::memory_order_relaxed);

    // Check if this is the last element
    if (idx == ExtractIndex(h)) {
        // Last element - need to update head atomically (stealers may be reading)
        uint64_t expected = h;
        if (head_.compare_exchange_strong(expected,
                MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            tail_.store(MakeVal(ExtractVersion(t) + 1, (idx + 1) % CAPACITY),
                        std::memory_order_relaxed);
            return task;
        }
        return nullptr;  // Stealer got it
    }

    // Not last element - just update tail (relaxed, owner exclusive)
    tail_.store(MakeVal(ExtractVersion(t) + 1, idx), std::memory_order_relaxed);
    return task;
}
```

- [ ] **Step 3: Update Worker::PickTask to use PopOwner**

```cpp
// src/bthread/core/worker.cpp around line 162

TaskMetaBase* Worker::PickTask() {
    // 1. Try batch first (LIFO for cache locality)
    if (batch_count_ > 0) {
        return local_batch_[--batch_count_];
    }

    // 2. Try local queue - use PopOwner for exclusive access optimization
    int popped = local_queue_.PopOwnerMultiple(local_batch_, BATCH_SIZE);
    if (popped > 0) {
        batch_count_ = popped;
        return local_batch_[--batch_count_];
    }
    // ... rest unchanged
}
```

- [ ] **Step 4: Add PopOwnerMultiple for batch operations**

```cpp
// include/bthread/queue/work_stealing_queue.hpp
int PopOwnerMultiple(TaskMetaBase** buffer, int max_count);

// src/bthread/queue/work_stealing_queue.cpp
int WorkStealingQueue::PopOwnerMultiple(TaskMetaBase** buffer, int max_count) {
    int count = 0;
    while (count < max_count) {
        TaskMetaBase* task = PopOwner();
        if (!task) break;
        buffer[count++] = task;
    }
    return count;
}
```

- [ ] **Step 5: Build and test**

```bash
cd build && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass

- [ ] **Step 6: Run benchmark**

```bash
./benchmark/benchmark
```

Expected: Create/Join improves 5-10%

- [ ] **Step 7: Commit**

```bash
git add include/bthread/queue/work_stealing_queue.hpp src/bthread/queue/work_stealing_queue.cpp src/bthread/core/worker.cpp
git commit -m "perf(queue): add owner-fast-path PopOwner with relaxed atomics

Owner has exclusive access to queue tail, can use relaxed memory ordering.
This reduces atomic operation overhead in the hot path.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 3: Batch Wake Optimization in Scheduler

**Files:**
- Modify: `include/bthread/core/scheduler.hpp:70-80`
- Modify: `src/bthread/core/scheduler.cpp:145-170`

**Problem:** Submit() wakes one idle worker per task. When submitting multiple tasks in burst, this causes multiple wake syscalls.

**Solution:** Add `SubmitMultiple` interface that wakes workers once after batch submission.

- [ ] **Step 1: Add SubmitMultiple method declaration**

```cpp
// include/bthread/core/scheduler.hpp

/**
 * @brief Submit multiple tasks at once - more efficient than individual Submit.
 * Wakes idle workers only once after batch, reducing syscall overhead.
 */
void SubmitMultiple(TaskMetaBase** tasks, int count);
```

- [ ] **Step 2: Implement SubmitMultiple**

```cpp
// src/bthread/core/scheduler.cpp

void Scheduler::SubmitMultiple(TaskMetaBase** tasks, int count) {
    // Auto-initialize if not already initialized
    if (!initialized_.load(std::memory_order_acquire)) {
        Init();
    }

    // Set all tasks to READY
    for (int i = 0; i < count; ++i) {
        tasks[i]->state.store(TaskState::READY, std::memory_order_release);
    }

    Worker* current = Worker::Current();
    if (current) {
        // Push all to current worker's local queue
        for (int i = 0; i < count; ++i) {
            current->local_queue().Push(tasks[i]);
        }
        // Wake one idle worker after batch
        WakeIdleWorkers(1);
    } else {
        // Push all to global queue, wake min(count, worker_count) workers
        for (int i = 0; i < count; ++i) {
            global_queue_.Push(tasks[i]);
        }
        // Wake multiple workers proportional to batch size
        WakeIdleWorkers(std::min(count, worker_count_.load(std::memory_order_acquire)));
    }
}
```

- [ ] **Step 3: Add benchmark for batch submission**

```cpp
// benchmark/benchmark.cpp

void benchmark_batch_submit(int batch_size, int iterations) {
    fprintf(stderr, "\n[Benchmark 8] Batch Submit Performance\n");
    fprintf(stderr, "  Batch size: %d, Iterations: %d\n", batch_size, iterations);

    std::vector<bthread_t> tids(batch_size);

    Timer timer;
    for (int i = 0; i < iterations; ++i) {
        // Create batch
        for (int j = 0; j < batch_size; ++j) {
            bthread_create(&tids[j], nullptr, empty_task, nullptr);
        }
        // Join all
        for (int j = 0; j < batch_size; ++j) {
            bthread_join(tids[j], nullptr);
        }
    }

    double elapsed = timer.elapsed_ms();
    int total_ops = batch_size * iterations;
    double ops_per_sec = total_ops / (elapsed / 1000.0);

    fprintf(stderr, "  Total time: %.2f ms\n", elapsed);
    fprintf(stderr, "  Throughput: %.0f ops/sec\n", ops_per_sec);
}
```

- [ ] **Step 4: Build and test**

```bash
cd build && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add include/bthread/core/scheduler.hpp src/bthread/core/scheduler.cpp
git commit -m "perf(scheduler): add SubmitMultiple for batch task submission

Batch submission wakes workers once after all tasks are queued,
reducing syscall overhead for burst workloads.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 4: Adaptive Spin Tuning for Worker WaitForTask

**Files:**
- Modify: `src/bthread/core/worker.cpp:222-279`

**Problem:** Current spin count (50) is fixed. Different workloads benefit from different spin parameters.

**Solution:** Make spin parameters adaptive based on recent wake patterns.

- [ ] **Step 1: Add adaptive spin tracking fields**

```cpp
// include/bthread/core/worker.hpp

private:
    // Adaptive spin tuning
    std::atomic<int> recent_wakes_{0};  // Wake count in last second
    int adaptive_spin_max_{50};         // Current max spin count

    // Track wake frequency for adaptive tuning
    int64_t last_wake_check_time_{0};
```

- [ ] **Step 2: Update WaitForTask with adaptive logic**

```cpp
// src/bthread/core/worker.cpp

void Worker::WaitForTask() {
    // Adaptive spin parameters based on recent wake frequency
    // High wake frequency = aggressive spin (tasks coming fast)
    // Low wake frequency = conservative spin (few tasks, save CPU)
    int wakes = recent_wakes_.load(std::memory_order_relaxed);
    int max_spins = (wakes > 10) ? 100 :  // High activity: spin more
                   (wakes > 3)  ? 50  :   // Medium: default
                                  25;    // Low activity: spin less

    constexpr int SPIN_CHECK_INTERVAL = 5;
    platform::timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;

    int spin_count = 0;

    while (stop_flag_.load(std::memory_order_seq_cst) == 0) {
        if (!local_queue_.Empty() ||
            !Scheduler::Instance().global_queue().Empty()) {
            return;
        }

        if (spin_count < max_spins) {
            #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
            #else
            std::atomic_signal_fence(std::memory_order_acquire);
            #endif
            ++spin_count;
            if (spin_count % SPIN_CHECK_INTERVAL == 0) {
                if (!local_queue_.Empty() ||
                    !Scheduler::Instance().global_queue().Empty()) {
                    return;
                }
            }
            continue;
        }

        is_idle_.store(true, std::memory_order_release);
        int expected = wake_count_.load(std::memory_order_acquire);
        platform::FutexWait(&wake_count_, expected, &ts);
        is_idle_.store(false, std::memory_order_release);

        // Track wake for adaptive tuning
        recent_wakes_.fetch_add(1, std::memory_order_relaxed);

        spin_count = 0;
    }
}
```

- [ ] **Step 3: Add periodic wake tracking reset**

Add a timer or periodic check to reset recent_wakes_:

```cpp
// In Worker::Run(), add periodic wake tracking decay
// Every 1000 tasks, decay the wake counter
if (task_count % 1000 == 0) {
    recent_wakes_.store(recent_wakes_.load(std::memory_order_relaxed) / 2,
                        std::memory_order_relaxed);
}
```

- [ ] **Step 4: Build and test**

```bash
cd build && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add include/bthread/core/worker.hpp src/bthread/core/worker.cpp
git commit -m "perf(worker): add adaptive spin tuning based on wake frequency

High wake frequency -> aggressive spin (100 iterations)
Low wake frequency -> conservative spin (25 iterations)
This balances CPU usage vs latency for different workloads.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 5: Performance Validation and Documentation Update

**Files:**
- Modify: `docs/performance_history.md`
- Modify: `docs/performance_optimization.md`

- [ ] **Step 1: Run full benchmark suite**

```bash
./benchmark/benchmark
```

Record all metrics.

- [ ] **Step 2: Update performance_history.md**

Add new entry for Phase 3 optimizations with before/after metrics.

- [ ] **Step 3: Update performance_optimization.md**

Add new optimization details and update metric tables.

- [ ] **Step 4: Commit documentation**

```bash
git add docs/performance_history.md docs/performance_optimization.md
git commit -m "docs: update performance metrics with Phase 3 optimization results

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Expected Performance Impact

| Metric | Before | After Target | Improvement |
|--------|--------|--------------|-------------|
| Create/Join | 83K | 90K+ | +8% |
| Scalability (8w) | 9.49x | 10x+ | +5% |
| Mutex Contention | 12M | 13M+ | +8% |

---

## Risks and Mitigation

| Risk | Mitigation |
|------|------------|
| Cache line alignment increases memory usage | Bounded - only 3 atomic fields per worker |
| PopOwner race with stealer | CAS on last element handles race correctly |
| Adaptive spin oscillation | Decay factor prevents wild swings |
| Batch wake wakes too many workers | Wake min(batch_size, worker_count) |

---

## Dependencies Between Tasks

- Task 1 (Cache alignment) - independent
- Task 2 (PopOwner) - independent
- Task 3 (SubmitMultiple) - independent
- Task 4 (Adaptive spin) - independent
- Task 5 (Documentation) - depends on Tasks 1-4

All implementation tasks are independent and can be executed in parallel or in sequence.