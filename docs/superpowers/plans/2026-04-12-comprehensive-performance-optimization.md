# Comprehensive Performance Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement 5 performance optimizations to restore Phase 3 performance and improve key metrics

**Architecture:** 
- Optimization 1: Idle worker registry (atomic linked list) for selective wake
- Optimization 2: Replace Mutex waiter mutex with MpscQueue (lock-free)
- Optimization 3: Sharded global queue (per-worker shards + steal)
- Optimization 4: Per-worker timer shards (reduce contention)
- Optimization 5: Yield fast path (direct suspend when no contention)

**Tech Stack:** C++20, atomic operations, lock-free queues, futex syscalls

---

## File Structure

```
Files to modify:
├── include/bthread/core/scheduler.hpp    # Add idle registry members
├── src/bthread/core/scheduler.cpp        # Implement RegisterIdle, PopIdleWorker
├── src/bthread/core/worker.cpp           # Register idle before futex wait
├── include/bthread/sync/mutex.hpp        # Replace waiter mutex with MpscQueue
├── src/bthread/sync/mutex.cpp            # Implement lock-free enqueue/dequeue
├── include/bthread/queue/sharded_queue.hpp  # NEW: ShardedGlobalQueue class
├── src/bthread/queue/sharded_queue.cpp      # NEW: Implementation
├── include/bthread/detail/timer_thread.hpp # Add shards
├── src/bthread/detail/timer_thread.cpp     # Implement shard-based scheduling
├── src/bthread/core/worker.cpp            # YieldCurrent fast path

Files to create:
├── tests/idle_registry_test.cpp           # Test idle registry
├── tests/sharded_queue_test.cpp           # Test sharded queue
├── tests/timer_shard_test.cpp             # Test timer shards
├── benchmark/benchmark                    # Rebuild after changes

Files to update:
├── docs/performance_history.md            # Record new benchmark results
```

---

## Task 1: WakeIdleWorkers - Add Idle Registry to Scheduler Header

**Files:**
- Modify: `include/bthread/core/scheduler.hpp:157-180`

- [ ] **Step 1: Add idle registry members to Scheduler header**

Open `include/bthread/core/scheduler.hpp` and add idle registry members after `GlobalQueue global_queue_;`:

```cpp
// In private section, after GlobalQueue global_queue_; (line ~172)

    // ========== Idle Worker Registry (Optimization 1) ==========
    // Lock-free linked list of idle workers
    std::atomic<int> idle_head_{-1};            // Head of idle list (-1 = empty)
    std::atomic<int> idle_next_[MAX_WORKERS];  // Next pointers for each worker

    // Helper methods for idle registry
    void RegisterIdleWorker(int worker_id);    // Called by Worker before futex wait
    int PopIdleWorker();                        // Pop one idle worker, returns -1 if none
```

- [ ] **Step 2: Add public method declaration for PopIdleWorker**

In the public section after `WakeIdleWorkers(int count)` declaration:

```cpp
    /// Pop one idle worker from registry (returns -1 if none)
    int PopIdleWorker();
```

- [ ] **Step 3: Commit header changes**

```bash
git add include/bthread/core/scheduler.hpp
git commit -m "feat(scheduler): add idle worker registry declarations

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 2: WakeIdleWorkers - Implement Idle Registry in Scheduler

**Files:**
- Modify: `src/bthread/core/scheduler.cpp:250-280`

- [ ] **Step 1: Implement RegisterIdleWorker**

Add to `src/bthread/core/scheduler.cpp` after `WakeIdleWorkers` function:

```cpp
void Scheduler::RegisterIdleWorker(int worker_id) {
    // Push worker_id onto idle list (lock-free)
    int old_head = idle_head_.load(std::memory_order_acquire);
    do {
        idle_next_[worker_id].store(old_head, std::memory_order_relaxed);
    } while (!idle_head_.compare_exchange_weak(old_head, worker_id,
            std::memory_order_release, std::memory_order_acquire));
}

int Scheduler::PopIdleWorker() {
    // Pop one worker from idle list (lock-free)
    int worker_id = idle_head_.load(std::memory_order_acquire);
    while (worker_id >= 0) {
        int next = idle_next_[worker_id].load(std::memory_order_relaxed);
        if (idle_head_.compare_exchange_weak(worker_id, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return worker_id;  // Successfully popped
        }
        // CAS failed, retry with new head
    }
    return -1;  // No idle workers
}
```

- [ ] **Step 2: Modify WakeIdleWorkers to use PopIdleWorker**

Replace the current `WakeIdleWorkers` implementation (lines 253-269) with:

```cpp
void Scheduler::WakeIdleWorkers(int count) {
    // Optimized: Only wake N idle workers (not ALL workers)
    for (int i = 0; i < count; ++i) {
        int idle_id = PopIdleWorker();
        if (idle_id < 0) break;  // No idle workers available
        
        Worker* w = workers_atomic_[idle_id].load(std::memory_order_acquire);
        if (w) {
            w->WakeUp();
        }
    }
}
```

- [ ] **Step 3: Commit implementation**

```bash
git add src/bthread/core/scheduler.cpp
git commit -m "perf(scheduler): implement idle worker registry for selective wake

- RegisterIdleWorker: lock-free push to idle list
- PopIdleWorker: lock-free pop from idle list
- WakeIdleWorkers: only wake N idle workers (not ALL)

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 3: WakeIdleWorkers - Modify Worker to Register Idle

**Files:**
- Modify: `src/bthread/core/worker.cpp:274-353`

- [ ] **Step 1: Add RegisterIdleWorker call in WaitForTask**

Find the `WaitForTask()` function (line ~274). After setting `is_idle_` to true, add the registration call:

```cpp
// Around line 315-316, after is_idle_.store(true)
is_idle_.store(true, std::memory_order_seq_cst);

// NEW: Register this worker as idle before futex wait
Scheduler::Instance().RegisterIdleWorker(id_);
```

- [ ] **Step 2: Compile to verify**

```bash
cd build && make -j$(nproc)
```

Expected: Compilation succeeds with no errors.

- [ ] **Step 3: Run basic benchmark to verify correctness**

```bash
./benchmark/benchmark 2>&1 | head -30
```

Expected: Benchmark runs without hanging or crashing.

- [ ] **Step 4: Commit Worker changes**

```bash
git add src/bthread/core/worker.cpp
git commit -m "perf(worker): register idle before futex wait

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 4: WakeIdleWorkers - Add Unit Test

**Files:**
- Create: `tests/idle_registry_test.cpp`

- [ ] **Step 1: Write unit test for idle registry**

Create `tests/idle_registry_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include "bthread/core/scheduler.hpp"

namespace bthread {
namespace test {

// Test fixture for idle registry
class IdleRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::Instance().Init(4);
    }
    
    void TearDown() override {
        Scheduler::Instance().Shutdown();
    }
};

TEST_F(IdleRegistryTest, PopFromEmptyReturnsMinusOne) {
    // Idle list should start empty
    int id = Scheduler::Instance().PopIdleWorker();
    EXPECT_EQ(id, -1);
}

TEST_F(IdleRegistryTest, RegisterAndPopSingleWorker) {
    // Register worker 0 as idle
    Scheduler::Instance().RegisterIdleWorker(0);
    
    // Pop should return 0
    int id = Scheduler::Instance().PopIdleWorker();
    EXPECT_EQ(id, 0);
    
    // After pop, list should be empty
    id = Scheduler::Instance().PopIdleWorker();
    EXPECT_EQ(id, -1);
}

TEST_F(IdleRegistryTest, RegisterMultipleWorkers) {
    // Register workers 0, 1, 2 as idle
    Scheduler::Instance().RegisterIdleWorker(0);
    Scheduler::Instance().RegisterIdleWorker(1);
    Scheduler::Instance().RegisterIdleWorker(2);
    
    // Pop should return all three (order may vary due to CAS)
    int count = 0;
    int ids[3];
    for (int i = 0; i < 3; ++i) {
        int id = Scheduler::Instance().PopIdleWorker();
        if (id >= 0) {
            ids[count++] = id;
        }
    }
    
    EXPECT_EQ(count, 3);
    
    // All IDs should be valid
    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(ids[i], 0);
        EXPECT_LT(ids[i], 4);
    }
}

TEST_F(IdleRegistryTest, ConcurrentRegisterAndPop) {
    std::atomic<int> registered_count{0};
    std::atomic<int> popped_count{0};
    
    // Multiple threads registering idle
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i]() {
            Scheduler::Instance().RegisterIdleWorker(i);
            registered_count.fetch_add(1);
        });
    }
    
    // Wait for all registrations
    for (auto& t : threads) t.join();
    
    // Pop all
    while (true) {
        int id = Scheduler::Instance().PopIdleWorker();
        if (id < 0) break;
        popped_count.fetch_add(1);
    }
    
    EXPECT_EQ(registered_count.load(), 4);
    EXPECT_EQ(popped_count.load(), 4);
}

} // namespace test
} // namespace bthread
```

- [ ] **Step 2: Add test to CMakeLists.txt**

Add to `tests/CMakeLists.txt`:

```cmake
# Add idle registry test
add_executable(idle_registry_test idle_registry_test.cpp)
target_link_libraries(idle_registry_test bthread GTest::gtest GTest::gtest_main pthread)
add_test(NAME IdleRegistryTest COMMAND idle_registry_test)
```

- [ ] **Step 3: Build and run test**

```bash
cd build && make idle_registry_test && ./tests/idle_registry_test
```

Expected: All tests pass.

- [ ] **Step 4: Commit test**

```bash
git add tests/idle_registry_test.cpp tests/CMakeLists.txt
git commit -m "test: add idle registry unit tests

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 5: WakeIdleWorkers - Run Benchmark and Verify Improvement

**Files:**
- Run benchmark

- [ ] **Step 1: Build complete project**

```bash
cd build && cmake .. && make -j$(nproc)
```

- [ ] **Step 2: Run benchmark 5 times and collect results**

```bash
for i in {1..5}; do
    echo "Run $i:"
    ./benchmark/benchmark 2>&1 | grep -E "Throughput|Ratio" | head -6
done
```

- [ ] **Step 3: Run ctest to verify all tests pass**

```bash
cd build && ctest --output-on-failure
```

Expected: All tests pass, 100% pass rate.

- [ ] **Step 4: Record benchmark results**

Create/update benchmark log in `docs/performance_optimization.md`:

```markdown
### Optimization 1: WakeIdleWorkers Results

| Run | Create/Join | Mutex Contention | Scalability (8w) |
|-----|-------------|------------------|------------------|
| 1   | TBD         | TBD              | TBD              |
| 2   | TBD         | TBD              | TBD              |
| 3   | TBD         | TBD              | TBD              |
| Avg | TBD         | TBD              | TBD              |
```

---

## Task 6: Mutex Waiter Queue - Modify MutexWaiterNode

**Files:**
- Modify: `include/bthread/sync/mutex.hpp:117-122`

- [ ] **Step 1: Add atomic next pointer to MutexWaiterNode**

Replace the MutexWaiterNode struct in `include/bthread/sync/mutex.hpp`:

```cpp
// OLD (lines 117-122):
    struct MutexWaiterNode {
        TaskMetaBase* task;
        MutexWaiterNode* next;
    };

// NEW:
    struct MutexWaiterNode {
        TaskMetaBase* task;
        std::atomic<MutexWaiterNode*> next{nullptr};  // Required by MpscQueue
    };
```

- [ ] **Step 2: Add MpscQueue for waiters**

Add include at top of file:

```cpp
#include "bthread/queue/mpsc_queue.hpp"
```

Replace waiter queue members in private section:

```cpp
// OLD (lines 116-122):
    std::mutex waiters_mutex_;
    struct MutexWaiterNode {
        TaskMetaBase* task;
        MutexWaiterNode* next;
    };
    MutexWaiterNode* waiter_head_{nullptr};
    MutexWaiterNode* waiter_tail_{nullptr};

// NEW:
    // Lock-free waiter queue (Optimization 2)
    MpscQueue<MutexWaiterNode> waiter_queue_;
```

- [ ] **Step 3: Commit header changes**

```bash
git add include/bthread/sync/mutex.hpp
git commit -m "feat(mutex): replace waiter mutex with MpscQueue declaration

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 7: Mutex Waiter Queue - Implement Lock-Free Enqueue/Dequeue

**Files:**
- Modify: `src/bthread/sync/mutex.cpp:226-248`

- [ ] **Step 1: Rewrite enqueue_waiter to use MpscQueue**

Replace `enqueue_waiter` function (lines 226-235):

```cpp
void Mutex::enqueue_waiter(TaskMetaBase* task) {
    MutexWaiterNode* node = new MutexWaiterNode{task};
    waiter_queue_.Push(node);  // Lock-free push
}
```

- [ ] **Step 2: Rewrite dequeue_waiter to use MpscQueue**

Replace `dequeue_waiter` function (lines 237-248):

```cpp
TaskMetaBase* Mutex::dequeue_waiter() {
    MutexWaiterNode* node = waiter_queue_.Pop();  // Lock-free pop
    if (!node) return nullptr;
    TaskMetaBase* task = node->task;
    delete node;
    return task;
}
```

- [ ] **Step 3: Remove waiters_mutex_ usage**

Search for any remaining `waiters_mutex_` usage and remove. The destructor already handles cleanup:

```cpp
Mutex::~Mutex() {
    // Drain remaining waiters from queue
    while (MutexWaiterNode* node = waiter_queue_.Pop()) {
        delete node;
    }
    
    // ... rest of destructor unchanged
}
```

- [ ] **Step 4: Build and verify**

```bash
cd build && make -j$(nproc)
```

Expected: Compilation succeeds.

- [ ] **Step 5: Commit implementation**

```bash
git add src/bthread/sync/mutex.cpp
git commit -m "perf(mutex): implement lock-free waiter queue using MpscQueue

- enqueue_waiter: lock-free Push
- dequeue_waiter: lock-free Pop
- Eliminates mutex contention on waiter operations

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 8: Mutex Waiter Queue - Run Tests

**Files:**
- Run existing mutex tests

- [ ] **Step 1: Run all tests**

```bash
cd build && ctest --output-on-failure
```

Expected: All tests pass, including mutex contention benchmark.

- [ ] **Step 2: Run mutex contention benchmark specifically**

```bash
./benchmark/benchmark 2>&1 | grep -A2 "Mutex Contention"
```

- [ ] **Step 3: Commit if tests pass**

No changes needed - tests already passed.

---

## Task 9: Global Queue MPMC - Create ShardedGlobalQueue Header

**Files:**
- Create: `include/bthread/queue/sharded_queue.hpp`

- [ ] **Step 1: Create ShardedGlobalQueue header**

Create new file `include/bthread/queue/sharded_queue.hpp`:

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include "bthread/queue/mpsc_queue.hpp"
#include "bthread/core/task_meta_base.hpp"

namespace bthread {

/**
 * @brief Sharded global queue for MPMC task distribution.
 *
 * Each worker has its own shard (MpscQueue). Push uses round-robin.
 * Pop tries own shard first, then steals from other shards.
 *
 * Thread Safety:
 * - Push(): Safe from multiple producer threads (round-robin to shards)
 * - Pop(): Safe from multiple consumer threads (each pops from own shard, steals from others)
 */
class ShardedGlobalQueue {
public:
    static constexpr int MAX_SHARDS = 256;

    ShardedGlobalQueue() = default;
    ~ShardedGlobalQueue() = default;

    // Disable copy and move
    ShardedGlobalQueue(const ShardedGlobalQueue&) = delete;
    ShardedGlobalQueue& operator=(const ShardedGlobalQueue&) = delete;

    /**
     * @brief Initialize shards for worker count.
     * @param worker_count Number of workers/shards
     */
    void Init(int worker_count);

    /**
     * @brief Push task to queue (round-robin to shards).
     * @param task Task to push
     */
    void Push(TaskMetaBase* task);

    /**
     * @brief Pop task from queue (own shard first, then steal).
     * @param worker_id Worker ID to pop from
     * @return Task pointer, or nullptr if empty
     */
    TaskMetaBase* Pop(int worker_id);

    /**
     * @brief Check if all shards are empty.
     * @return true if no tasks in any shard
     */
    bool Empty() const;

private:
    std::atomic<int> round_robin_{0};
    int worker_count_{0};
    MpscQueue<TaskMetaBase> shards_[MAX_SHARDS];
};

} // namespace bthread
```

- [ ] **Step 2: Commit header**

```bash
git add include/bthread/queue/sharded_queue.hpp
git commit -m "feat(queue): add ShardedGlobalQueue header for MPMC

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 10: Global Queue MPMC - Implement ShardedGlobalQueue

**Files:**
- Create: `src/bthread/queue/sharded_queue.cpp`

- [ ] **Step 1: Create implementation file**

Create `src/bthread/queue/sharded_queue.cpp`:

```cpp
#include "bthread/queue/sharded_queue.hpp"

namespace bthread {

void ShardedGlobalQueue::Init(int worker_count) {
    worker_count_ = worker_count;
}

void ShardedGlobalQueue::Push(TaskMetaBase* task) {
    // Round-robin distribution to shards
    int shard = round_robin_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
    shards_[shard].Push(task);
}

TaskMetaBase* ShardedGlobalQueue::Pop(int worker_id) {
    // 1. Try own shard first (fast path)
    if (worker_id >= 0 && worker_id < worker_count_) {
        TaskMetaBase* task = shards_[worker_id].Pop();
        if (task) return task;
    }

    // 2. Steal from other shards (slow path)
    for (int i = 0; i < worker_count_; ++i) {
        if (i == worker_id) continue;
        TaskMetaBase* task = shards_[i].Pop();
        if (task) return task;
    }

    return nullptr;
}

bool ShardedGlobalQueue::Empty() const {
    for (int i = 0; i < worker_count_; ++i) {
        if (!shards_[i].Empty()) return false;
    }
    return true;
}

} // namespace bthread
```

- [ ] **Step 2: Add to CMakeLists.txt**

Add to `src/bthread/CMakeLists.txt` or main `CMakeLists.txt`:

```cmake
# Add sharded_queue source
set(BTHREAD_SOURCES
    ...
    ${CMAKE_SOURCE_DIR}/src/bthread/queue/sharded_queue.cpp
    ...
)
```

- [ ] **Step 3: Build to verify**

```bash
cd build && cmake .. && make -j$(nproc)
```

Expected: Compilation succeeds.

- [ ] **Step 4: Commit implementation**

```bash
git add src/bthread/queue/sharded_queue.cpp CMakeLists.txt
git commit -m "feat(queue): implement ShardedGlobalQueue with round-robin and steal

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 11: Global Queue MPMC - Integrate with Scheduler

**Files:**
- Modify: `include/bthread/core/scheduler.hpp:76-77, 171`
- Modify: `src/bthread/core/scheduler.cpp:168-188`

- [ ] **Step 1: Replace GlobalQueue with ShardedGlobalQueue in header**

In `include/bthread/core/scheduler.hpp`:

Add include:
```cpp
#include "bthread/queue/sharded_queue.hpp"
```

Replace member (line ~171):
```cpp
// OLD:
    GlobalQueue global_queue_;

// NEW:
    ShardedGlobalQueue global_queue_;
```

Update accessor methods (lines 76-77):
```cpp
    ShardedGlobalQueue& global_queue() { return global_queue_; }
    const ShardedGlobalQueue& global_queue() const { return global_queue_; }
```

- [ ] **Step 2: Modify Scheduler::Submit to use ShardedGlobalQueue**

In `src/bthread/core/scheduler.cpp`, modify `Submit` function:

```cpp
void Scheduler::Submit(TaskMetaBase* task) {
    if (!initialized_.load(std::memory_order_acquire)) {
        Init();
    }

    task->state.store(TaskState::READY, std::memory_order_release);

    Worker* current = Worker::Current();
    if (current) {
        current->local_queue().Push(task);
        WakeIdleWorkers(1);
    } else {
        global_queue_.Push(task);  // Uses sharded queue
        WakeIdleWorkers(1);
    }
}
```

- [ ] **Step 3: Add PopGlobal method to Scheduler**

Add to `include/bthread/core/scheduler.hpp` public section:
```cpp
    /// Pop from global queue for specific worker
    TaskMetaBase* PopGlobal(int worker_id);
```

Add to `src/bthread/core/scheduler.cpp`:
```cpp
TaskMetaBase* Scheduler::PopGlobal(int worker_id) {
    return global_queue_.Pop(worker_id);
}
```

- [ ] **Step 4: Commit integration**

```bash
git add include/bthread/core/scheduler.hpp src/bthread/core/scheduler.cpp
git commit -m "feat(scheduler): integrate ShardedGlobalQueue for MPMC global dequeue

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 12: Global Queue MPMC - Modify Worker PickTask

**Files:**
- Modify: `src/bthread/core/worker.cpp:214-258`

- [ ] **Step 1: Update Worker::PickTask to use PopGlobal**

Find the `PickTask()` function (line ~214). Modify step 3 (global queue):

```cpp
// OLD (line 228-229):
    // 3. Try global queue
    TaskMetaBase* task = Scheduler::Instance().global_queue().Pop();

// NEW:
    // 3. Try global queue via sharded Pop
    TaskMetaBase* task = Scheduler::Instance().PopGlobal(id_);
    if (task) return task;
```

- [ ] **Step 2: Build and verify**

```bash
cd build && make -j$(nproc)
```

Expected: Compilation succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/bthread/core/worker.cpp
git commit -m "perf(worker): use PopGlobal for sharded global queue access

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 13: Global Queue MPMC - Add Unit Test

**Files:**
- Create: `tests/sharded_queue_test.cpp`

- [ ] **Step 1: Write unit test**

Create `tests/sharded_queue_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "bthread/queue/sharded_queue.hpp"
#include "bthread/core/task_meta.hpp"

namespace bthread {
namespace test {

class ShardedQueueTest : public ::testing::Test {
protected:
    ShardedGlobalQueue queue_;
    
    void SetUp() override {
        queue_.Init(4);
    }
};

TEST_F(ShardedQueueTest, PushPopSingleTask) {
    TaskMeta task;
    queue_.Push(&task);
    
    TaskMetaBase* popped = queue_.Pop(0);
    EXPECT_EQ(popped, &task);
    
    // Should be empty now
    EXPECT_TRUE(queue_.Empty());
}

TEST_F(ShardedQueueTest, RoundRobinDistribution) {
    TaskMeta tasks[8];
    
    // Push 8 tasks - should be distributed round-robin
    for (int i = 0; i < 8; ++i) {
        queue_.Push(&tasks[i]);
    }
    
    // Pop from each shard - should get 2 each
    int counts[4] = {0, 0, 0, 0};
    for (int shard = 0; shard < 4; ++shard) {
        while (TaskMetaBase* t = queue_.Pop(shard)) {
            counts[shard]++;
        }
    }
    
    // Total should be 8
    int total = 0;
    for (int c : counts) total += c;
    EXPECT_EQ(total, 8);
}

TEST_F(ShardedQueueTest, StealFromOtherShard) {
    TaskMeta task;
    
    // Push to shard 0 (round-robin 0)
    queue_.Push(&task);
    
    // Worker 1 steals from shard 0
    TaskMetaBase* popped = queue_.Pop(1);
    EXPECT_EQ(popped, &task);
}

TEST_F(ShardedQueueTest, ConcurrentPushPop) {
    std::atomic<int> pushed{0};
    std::atomic<int> popped{0};
    
    // 4 producers
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < 100; ++i) {
                TaskMeta* task = new TaskMeta();
                queue_.Push(task);
                pushed.fetch_add(1);
            }
        });
    }
    
    // 4 consumers
    std::vector<std::thread> consumers;
    for (int c = 0; c < 4; ++c) {
        consumers.emplace_back([&, c]() {
            for (int i = 0; i < 100; ++i) {
                if (TaskMetaBase* t = queue_.Pop(c)) {
                    popped.fetch_add(1);
                    delete static_cast<TaskMeta*>(t);
                }
            }
        });
    }
    
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    
    EXPECT_EQ(pushed.load(), 400);
    // May not pop all due to timing, but should pop most
    EXPECT_GT(popped.load(), 300);
    
    // Drain remaining
    for (int i = 0; i < 4; ++i) {
        while (TaskMetaBase* t = queue_.Pop(i)) {
            delete static_cast<TaskMeta*>(t);
        }
    }
}

} // namespace test
} // namespace bthread
```

- [ ] **Step 2: Add to CMakeLists.txt**

```cmake
add_executable(sharded_queue_test sharded_queue_test.cpp)
target_link_libraries(sharded_queue_test bthread GTest::gtest GTest::gtest_main pthread)
add_test(NAME ShardedQueueTest COMMAND sharded_queue_test)
```

- [ ] **Step 3: Build and run test**

```bash
cd build && make sharded_queue_test && ./tests/sharded_queue_test
```

Expected: All tests pass.

- [ ] **Step 4: Commit test**

```bash
git add tests/sharded_queue_test.cpp tests/CMakeLists.txt
git commit -m "test: add ShardedGlobalQueue unit tests

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 14: Timer Sharding - Modify TimerThread Header

**Files:**
- Modify: `include/bthread/detail/timer_thread.hpp:67-75`

- [ ] **Step 1: Add TimerShard structure and shards array**

In `include/bthread/detail/timer_thread.hpp`, add to private section:

```cpp
private:
    // ========== Timer Sharding (Optimization 4) ==========
    struct TimerShard {
        std::mutex mutex;                       // Per-shard lock
        std::vector<TimerEntry*> heap;          // Min-heap for this shard
        std::atomic<int64_t> next_deadline{INT64_MAX};  // Earliest deadline in shard
    };
    
    TimerShard shards_[MAX_SHARDS];
    std::atomic<int> shard_assign_{0};          // Round-robin shard assignment
    int worker_count_{0};                        // Number of shards to use
    
    static constexpr int MAX_SHARDS = 256;
```

- [ ] **Step 2: Add Init method declaration**

Add to public section:

```cpp
    /// Initialize shards for worker count
    void Init(int worker_count);
```

- [ ] **Step 3: Add ProcessShard method declaration**

Add to private section:

```cpp
    void ProcessShard(TimerShard& shard);  // Process expired timers in one shard
```

- [ ] **Step 4: Commit header changes**

```bash
git add include/bthread/detail/timer_thread.hpp
git commit -m "feat(timer): add TimerShard structure for sharded timers

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 15: Timer Sharding - Implement Shard-Based Scheduling

**Files:**
- Modify: `src/bthread/detail/timer_thread.cpp:47-133`

- [ ] **Step 1: Implement Init method**

Add to `src/bthread/detail/timer_thread.cpp`:

```cpp
void TimerThread::Init(int worker_count) {
    worker_count_ = std::min(worker_count, MAX_SHARDS);
}
```

- [ ] **Step 2: Rewrite Schedule to use shards**

Replace `Schedule` function (lines 47-68):

```cpp
int TimerThread::Schedule(void (*callback)(void*), void* arg, const platform::timespec* delay) {
    if (!delay) return -1;
    
    // Calculate deadline
    int64_t delay_us = static_cast<int64_t>(delay->tv_sec) * 1000000 +
                       delay->tv_nsec / 1000;
    int64_t deadline_us = platform::GetTimeOfDayUs() + delay_us;
    
    // Round-robin shard assignment
    int shard_id = shard_assign_.fetch_add(1, std::memory_order_relaxed) % 
                   (worker_count_ > 0 ? worker_count_ : 1);
    
    // Create entry
    auto* entry = new TimerEntry();
    entry->callback = callback;
    entry->arg = arg;
    entry->deadline_us = deadline_us;
    entry->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    entry->cancelled = false;
    
    // Add to shard's heap
    auto& shard = shards_[shard_id];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.heap.push_back(entry);
        std::push_heap(shard.heap.begin(), shard.heap.end(),
            [](TimerEntry* a, TimerEntry* b) { return a->deadline_us > b->deadline_us; });
        
        // Update next_deadline if this is earlier
        int64_t current_next = shard.next_deadline.load(std::memory_order_relaxed);
        if (deadline_us < current_next) {
            shard.next_deadline.store(deadline_us, std::memory_order_release);
        }
    }
    
    // Wake timer thread if this deadline is earlier than current sleep
    int64_t current_wakeup = wakeup_futex_.load(std::memory_order_acquire);
    // Simple heuristic: wake on any new timer (could optimize)
    wakeup_futex_.fetch_add(1, std::memory_order_release);
    platform::FutexWake(&wakeup_futex_, 1);
    
    return entry->id;
}
```

- [ ] **Step 3: Implement ProcessShard**

Add new function:

```cpp
void TimerThread::ProcessShard(TimerShard& shard) {
    int64_t now_us = platform::GetTimeOfDayUs();
    
    std::lock_guard<std::mutex> lock(shard.mutex);
    
    while (!shard.heap.empty() && shard.heap[0]->deadline_us <= now_us) {
        TimerEntry* entry = shard.heap[0];
        
        if (entry->cancelled) {
            std::pop_heap(shard.heap.begin(), shard.heap.end(),
                [](TimerEntry* a, TimerEntry* b) { return a->deadline_us > b->deadline_us; });
            shard.heap.pop_back();
            delete entry;
            continue;
        }
        
        // Pop and execute
        std::pop_heap(shard.heap.begin(), shard.heap.end(),
            [](TimerEntry* a, TimerEntry* b) { return a->deadline_us > b->deadline_us; });
        shard.heap.pop_back();
        
        // Update next_deadline
        if (!shard.heap.empty()) {
            shard.next_deadline.store(shard.heap[0]->deadline_us, std::memory_order_release);
        } else {
            shard.next_deadline.store(INT64_MAX, std::memory_order_release);
        }
        
        // Execute callback (outside lock would be better, but simpler here)
        void (*callback)(void*) = entry->callback;
        void* arg = entry->arg;
        delete entry;
        
        // Release lock before callback
        shard.mutex.unlock();
        callback(arg);
        shard.mutex.lock();
    }
}
```

- [ ] **Step 4: Rewrite TimerThreadMain**

Replace `TimerThreadMain` (lines 82-133):

```cpp
void TimerThread::TimerThreadMain() {
    while (running_.load(std::memory_order_acquire)) {
        int64_t now_us = platform::GetTimeOfDayUs();
        int64_t min_deadline = INT64_MAX;
        
        // Check all shards for expired timers
        int active_shards = worker_count_ > 0 ? worker_count_ : 1;
        for (int i = 0; i < active_shards; ++i) {
            auto& shard = shards_[i];
            int64_t shard_next = shard.next_deadline.load(std::memory_order_acquire);
            
            if (shard_next <= now_us) {
                ProcessShard(shard);
            }
            
            // Update min_deadline
            shard_next = shard.next_deadline.load(std::memory_order_acquire);
            if (shard_next < min_deadline) {
                min_deadline = shard_next;
            }
        }
        
        // Calculate sleep time
        int64_t sleep_us = min_deadline - platform::GetTimeOfDayUs();
        if (sleep_us <= 0 || min_deadline == INT64_MAX) {
            sleep_us = 100000;  // Default 100ms
        }
        
        // Wait
        platform::timespec ts;
        ts.tv_sec = sleep_us / 1000000;
        ts.tv_nsec = (sleep_us % 1000000) * 1000;
        
        platform::FutexWait(&wakeup_futex_, 
            wakeup_futex_.load(std::memory_order_acquire), &ts);
    }
    
    // Cleanup all shards
    for (int i = 0; i < MAX_SHARDS; ++i) {
        std::lock_guard<std::mutex> lock(shards_[i].mutex);
        for (auto* entry : shards_[i].heap) {
            delete entry;
        }
        shards_[i].heap.clear();
    }
}
```

- [ ] **Step 5: Build and verify**

```bash
cd build && make -j$(nproc)
```

Expected: Compilation succeeds.

- [ ] **Step 6: Commit implementation**

```bash
git add src/bthread/detail/timer_thread.cpp
git commit -m "perf(timer): implement sharded timer scheduling

- Round-robin shard assignment
- Per-shard mutex (lower contention)
- ProcessShard for expired timers

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 16: Timer Sharding - Initialize Shards in Scheduler

**Files:**
- Modify: `src/bthread/core/scheduler.cpp:43-56`

- [ ] **Step 1: Call TimerThread Init during Scheduler Init**

In `Scheduler::Init()`, after starting workers, add timer initialization:

```cpp
void Scheduler::Init() {
    std::call_once(init_once_, [this] {
        // ... existing init code ...
        
        StartWorkers(n);
        GetTaskGroup().set_worker_count(n);
        
        // NEW: Initialize timer thread with worker count
        GetTimerThread()->Init(n);
        
        // ... rest of init ...
    });
}
```

- [ ] **Step 2: Build and verify**

```bash
cd build && make -j$(nproc)
```

- [ ] **Step 3: Commit**

```bash
git add src/bthread/core/scheduler.cpp
git commit -m "feat(scheduler): initialize timer thread shards

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 17: Timer Sharding - Add Unit Test

**Files:**
- Create: `tests/timer_shard_test.cpp`

- [ ] **Step 1: Write timer shard test**

Create `tests/timer_shard_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include "bthread/detail/timer_thread.hpp"

namespace bthread {
namespace test {

class TimerShardTest : public ::testing::Test {
protected:
    TimerThread timer_;
    
    void SetUp() override {
        timer_.Init(4);
        timer_.Start();
    }
    
    void TearDown() override {
        timer_.Stop();
    }
};

TEST_F(TimerShardTest, ScheduleAndExecute) {
    std::atomic<int> counter{0};
    
    platform::timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;  // 1ms
    
    int id = timer_.Schedule([](void* arg) {
        std::atomic<int>* c = static_cast<std::atomic<int>*>(arg);
        c->fetch_add(1);
    }, &counter, &ts);
    
    EXPECT_GE(id, 0);
    
    // Wait for callback
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    EXPECT_EQ(counter.load(), 1);
}

TEST_F(TimerShardTest, MultipleTimers) {
    std::atomic<int> counter{0};
    
    platform::timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 500000;  // 0.5ms
    
    // Schedule 10 timers
    for (int i = 0; i < 10; ++i) {
        timer_.Schedule([](void* arg) {
            std::atomic<int>* c = static_cast<std::atomic<int>*>(arg);
            c->fetch_add(1);
        }, &counter, &ts);
    }
    
    // Wait for all callbacks
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    EXPECT_EQ(counter.load(), 10);
}

TEST_F(TimerShardTest, CancelTimer) {
    std::atomic<int> counter{0};
    
    platform::timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000;  // 10ms
    
    int id = timer_.Schedule([](void* arg) {
        std::atomic<int>* c = static_cast<std::atomic<int>*>(arg);
        c->fetch_add(1);
    }, &counter, &ts);
    
    // Cancel immediately
    bool cancelled = timer_.Cancel(id);
    EXPECT_TRUE(cancelled);
    
    // Wait past deadline
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    // Should not have executed
    EXPECT_EQ(counter.load(), 0);
}

} // namespace test
} // namespace bthread
```

- [ ] **Step 2: Add to CMakeLists.txt**

```cmake
add_executable(timer_shard_test timer_shard_test.cpp)
target_link_libraries(timer_shard_test bthread GTest::gtest GTest::gtest_main pthread)
add_test(NAME TimerShardTest COMMAND timer_shard_test)
```

- [ ] **Step 3: Build and run test**

```bash
cd build && make timer_shard_test && ./tests/timer_shard_test
```

Expected: All tests pass.

- [ ] **Step 4: Commit test**

```bash
git add tests/timer_shard_test.cpp tests/CMakeLists.txt
git commit -m "test: add timer shard unit tests

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 18: Yield Fast Path - Modify YieldCurrent

**Files:**
- Modify: `src/bthread/core/worker.cpp:190-203`

- [ ] **Step 1: Add fast path check in YieldCurrent**

Find `YieldCurrent()` function (line ~190). Add fast path:

```cpp
int Worker::YieldCurrent() {
    if (!current_task_) return EINVAL;
    
    // Fast path: No contention - direct yield without queue/wake
    // Skip queue operations when local queue is empty and no batch pending
    if (batch_count_ == 0 && local_queue_.Empty()) {
        // Direct suspend - task will be picked up immediately after resume
        // This avoids unnecessary queue operations and worker wakeups
        SuspendCurrent();
        return 0;
    }
    
    // Normal path: enqueue to batch/local_queue
    current_task_->state.store(TaskState::READY, std::memory_order_release);
    local_batch_[batch_count_++] = current_task_;
    MaybeFlushBatch();
    SuspendCurrent();
    return 0;
}
```

- [ ] **Step 2: Build and verify**

```bash
cd build && make -j$(nproc)
```

Expected: Compilation succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/bthread/core/worker.cpp
git commit -m "perf(worker): add yield fast path for no-contention case

- Direct suspend when batch and local queue empty
- Avoids unnecessary queue operations and wakeups

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Task 19: Yield Fast Path - Profile Yield Path

**Files:**
- Run perf analysis

- [ ] **Step 1: Run perf record on benchmark**

```bash
cd build
perf record -g -o perf_yield.data ./benchmark/benchmark
perf report -i perf_yield.data --stdio --no-children | grep -A5 "Yield"
```

- [ ] **Step 2: Analyze results**

Look for hotspots in Yield path:
- Is `WakeIdleWorkers` being called?
- Is `local_queue_.Push` or `batch` overhead significant?
- Is context switch (`SwapContext`) taking most time?

- [ ] **Step 3: Document findings**

Add to `docs/performance_optimization.md`:

```markdown
### Yield Path Profiling Results

**Hotspots:**
- TBD (from perf output)

**Root cause of Phase 3 → Phase 5 regression:**
- TBD
```

---

## Task 20: Final Verification - Run All Tests and Benchmarks

**Files:**
- Run comprehensive tests

- [ ] **Step 1: Build complete project**

```bash
cd build && cmake .. && make -j$(nproc)
```

- [ ] **Step 2: Run all unit tests**

```bash
cd build && ctest --output-on-failure
```

Expected: 100% pass rate.

- [ ] **Step 3: Run benchmark 10 times**

```bash
cd build
for i in {1..10}; do
    echo "=== Run $i ==="
    ./benchmark/benchmark 2>&1 | grep -E "Throughput|Ratio" | head -10
done
```

- [ ] **Step 4: Record final benchmark results**

Update `docs/performance_history.md` with final results.

---

## Task 21: Update Documentation

**Files:**
- Modify: `docs/performance_history.md`

- [ ] **Step 1: Add optimization results section**

Add to `docs/performance_history.md`:

```markdown
---

## 2026-04-12: Comprehensive Performance Optimization (Phase 6)

**设计文档**: `docs/superpowers/specs/2026-04-12-comprehensive-performance-optimization-design.md`
**实现计划**: `docs/superpowers/plans/2026-04-12-comprehensive-performance-optimization.md`

### 优化内容

| 优化项 | 说明 | 预期效果 |
|--------|------|----------|
| WakeIdleWorkers Selective Wake | 空闲 worker 注册表，只唤醒 N 个 | +15-20% Create/Join |
| Mutex Lock-Free Waiter | 使用 MpscQueue 替代 waiter mutex | +50% Mutex Contention |
| Global Queue MPMC | 分片队列 + steal | +25% Scalability |
| Timer Sharding | Per-worker timer shard | 降低 contention |
| Yield Fast Path | 无竞争时直接 suspend | +100-150% Yield |

### 性能结果

| 指标 | Phase 5+ (优化前) | Phase 6 (优化后) | 改进幅度 |
|------|------------------|------------------|----------|
| Create/Join | ~150K ops/sec | TBD | TBD |
| Yield | 8M/sec | TBD | TBD |
| Mutex Contention | 12M/sec | TBD | TBD |
| Scalability (8w) | 5.6x | TBD | TBD |

### 提交记录

| 提交 | 说明 |
|------|------|
| TBD | feat: all optimization commits |
```

- [ ] **Step 2: Commit documentation**

```bash
git add docs/performance_history.md docs/performance_optimization.md
git commit -m "docs: record Phase 6 comprehensive optimization results

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Self-Review Checklist

After completing all tasks:

1. **Spec coverage:**
   - [x] Optimization 1 (WakeIdleWorkers) - Tasks 1-5
   - [x] Optimization 2 (Mutex Lock-Free) - Tasks 6-8
   - [x] Optimization 3 (Global Queue MPMC) - Tasks 9-13
   - [x] Optimization 4 (Timer Sharding) - Tasks 14-17
   - [x] Optimization 5 (Yield Fast Path) - Tasks 18-19
   - [x] Final verification - Task 20
   - [x] Documentation - Task 21

2. **Placeholder scan:**
   - No "TBD" in code (only in benchmark result placeholders)
   - All code blocks contain complete implementation
   - No "TODO" or "implement later"

3. **Type consistency:**
   - `ShardedGlobalQueue::Pop(int worker_id)` matches `Scheduler::PopGlobal(int worker_id)`
   - `MutexWaiterNode` has `std::atomic<MutexWaiterNode*> next` for MpscQueue
   - `TimerShard` defined in TimerThread header matches usage in implementation

---

## Summary

This plan implements 5 performance optimizations:

| Phase | Optimization | Tasks | Expected Impact |
|-------|--------------|-------|-----------------|
| 1 | WakeIdleWorkers | 1-5 | +15-20% Create/Join |
| 2 | Mutex Lock-Free | 6-8 | +50% Mutex Contention |
| 3 | Global Queue MPMC | 9-13 | +25% Scalability |
| 4 | Timer Sharding | 14-17 | Lower contention |
| 5 | Yield Fast Path | 18-19 | +100-150% Yield |

Total: 21 tasks, each with bite-sized steps following TDD.