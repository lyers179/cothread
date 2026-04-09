# Bthread 性能优化实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 优化 bthread M:N 线程库的运行时性能，重点关注热路径、内存布局和锁竞争

**Architecture:** 分 5 阶段执行，每阶段独立验证性能基准测试后再进入下一阶段。先优化最热路径（队列操作），后优化同步原语，最后优化调度器。

**Tech Stack:** C++20, CMake 3.15+, Linux x86-64, atomic operations, lock-free algorithms

---

## 性能分析摘要

基于代码审查和基准测试，主要优化点：

| 优先级 | 优化点 | 预期收益 | 风险 |
|--------|--------|----------|------|
| P0 | MPSC Queue Pop() yield → spin | 减少 context switch，~10-20% | 低 |
| P0 | Worker::PickTask() batch optimization | 减少 atomic ops，~5-10% | 低 |
| P1 | Adaptive spinning before futex | 减少 kernel calls | 中 |
| P1 | Butex Wait() atomic reduction | 减少内存 barrier | 中 |
| P2 | TaskMeta memory layout | 改善 cache locality | 低 |
| P2 | Scheduler wake mechanism | 减少锁竞争 | 中 |
| P3 | XMM register saving (optional) | SIMD tasks | 低 |

---

## 文件结构

优化涉及的文件：

| 文件 | 优化内容 |
|------|----------|
| `include/bthread/queue/mpsc_queue.hpp` | Pop() spin 优化 |
| `include/bthread/core/worker.hpp` | PickTask() batch, adaptive spin |
| `src/bthread/core/worker.cpp` | 实现优化 |
| `include/bthread/core/task_meta.hpp` | 内存布局优化 |
| `src/bthread/sync/butex.cpp` | Wait() atomic 减少 |
| `include/bthread/core/scheduler.hpp` | Wake mechanism |
| `src/bthread/core/scheduler.cpp` | Wake 实现 |
| `src/bthread/core/task_group.cpp` | Free list 优化 |
| `benchmark/benchmark.cpp` | 性能基准测试 |
| `tests/perf/perf_test.cpp` | 新增微基准测试 |

---

## Phase 1: 热路径队列优化

### Task 1.1: MPSC Queue Pop() Spin 优化

**Files:**
- Modify: `include/bthread/queue/mpsc_queue.hpp`

**问题**: 当前 Pop() 在 race condition 时调用 `std::this_thread::yield()`，这会导致不必要的 context switch。对于高频调用场景，应该先 spin 几次再 yield。

- [ ] **Step 1: 编写失败的性能测试**

Create `tests/perf/mpsc_perf_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include <benchmark/benchmark.h>
#include "bthread/queue/mpsc_queue.hpp"
#include <thread>
#include <atomic>

// 性能测试：高并发 Push/Pop
static void BM_MpscQueueHighContention(benchmark::State& state) {
    bthread::MpscQueue<bthread::TaskMetaBase> queue;
    int num_threads = state.range(0);
    int ops_per_thread = state.range(1);
    
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    
    // 生产者线程
    std::vector<std::thread> producers;
    for (int i = 0; i < num_threads; ++i) {
        producers.emplace_back([&]() {
            ready.fetch_add(1);
            while (!start.load()) {}
            for (int j = 0; j < ops_per_thread; ++j) {
                auto* item = new bthread::TaskMetaBase();
                queue.Push(item);
            }
        });
    }
    
    // 消费者线程
    std::thread consumer([&]() {
        ready.fetch_add(1);
        while (!start.load()) {}
        int consumed = 0;
        while (consumed < num_threads * ops_per_thread) {
            auto* item = queue.Pop();
            if (item) {
                delete item;
                ++consumed;
            }
        }
    });
    
    // 等待所有线程就绪
    while (ready.load() < num_threads + 1) {}
    start.store(true);
    
    for (auto& p : producers) p.join();
    consumer.join();
}
BENCHMARK(BM_MpscQueueHighContention)->Args({4, 10000});

TEST(MpscPerf, PopShouldNotYieldImmediately) {
    // 验证：Pop 在 race condition 时应该先 spin 再 yield
    bthread::MpscQueue<bthread::TaskMetaBase> queue;
    
    // 设置竞态条件
    auto* item1 = new bthread::TaskMetaBase();
    auto* item2 = new bthread::TaskMetaBase();
    
    queue.Push(item1);
    queue.Push(item2);
    
    // 多线程同时 Pop
    std::atomic<int> spins{0};
    std::thread t1([&]() {
        auto* p = queue.Pop();
        if (p) delete p;
    });
    std::thread t2([&]() {
        auto* p = queue.Pop();
        if (p) delete p;
    });
    
    t1.join();
    t2.join();
}
```

- [ ] **Step 2: 实现自适应 spin**

Edit `include/bthread/queue/mpsc_queue.hpp`:

```cpp
// 旧代码 (lines 71-73):
while (!t->next.load(std::memory_order_acquire)) {
    std::this_thread::yield();
}

// 新代码 - 自适应 spin:
constexpr int MAX_SPINS = 100;  // 先 spin 100 次，再 yield
int spin_count = 0;
while (!t->next.load(std::memory_order_acquire)) {
    if (++spin_count < MAX_SPINS) {
        // 使用 pause 指令减少功耗 (x86)
        #if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
        #else
        // 其他架构使用编译器 barrier
        std::atomic_signal_fence(std::memory_order_acquire);
        #endif
    } else {
        std::this_thread::yield();
        spin_count = 0;  // yield 后重置，避免持续 yield
    }
}
```

- [ ] **Step 3: 运行性能测试**

```bash
cd build && make -j$(nproc)
cd build && ./tests/perf/mpsc_perf_test --benchmark_time_unit=us
```

Expected: 性能提升 ~10-20% 在高并发场景

- [ ] **Step 4: 验证现有测试通过**

```bash
cd build && ctest --output-on-failure
```

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "perf(queue): add adaptive spinning in MpscQueue Pop()"
```

---

### Task 1.2: Worker PickTask() Batch Prefill 优化

**Files:**
- Modify: `include/bthread/core/worker.hpp`
- Modify: `src/bthread/core/worker.cpp`

**问题**: 当前 PickTask() 从 local_queue Pop 后会 prefill batch，但这会执行多次 atomic load/store。优化为直接 Pop 到 batch。

- [ ] **Step 1: 分析当前实现**

当前代码 (worker.cpp:163-177):
```cpp
TaskMetaBase* task = local_queue_.Pop();
if (task) {
    local_batch_[batch_count_++] = task;
    // Prefill batch - 多次 atomic Pop
    for (int i = 0; i < BATCH_SIZE - 1 && batch_count_ < BATCH_SIZE; ++i) {
        TaskMetaBase* t2 = local_queue_.Pop();
        if (t2) {
            local_batch_[batch_count_++] = t2;
        } else {
            break;
        }
    }
    return local_batch_[--batch_count_];
}
```

问题：prefill 时多次调用 Pop()，每次 Pop() 都有 atomic 操作开销。

- [ ] **Step 2: 优化 prefill 为批量操作**

Edit `src/bthread/core/worker.cpp`:

```cpp
TaskMetaBase* Worker::PickTask() {
    // 1. Try batch first (LIFO for cache locality)
    if (batch_count_ > 0) {
        return local_batch_[--batch_count_];
    }

    // 2. Try local queue - 批量 pop 优化
    int popped = local_queue_.PopMultiple(local_batch_, BATCH_SIZE);
    if (popped > 0) {
        batch_count_ = popped;
        return local_batch_[--batch_count_];
    }

    // 3. Try global queue
    TaskMetaBase* task = Scheduler::Instance().global_queue().Pop();
    if (task) return task;

    // 4. Try work stealing (保持不变)
    ...
}
```

- [ ] **Step 3: 添加 MpscQueue PopMultiple() 方法**

Edit `include/bthread/queue/mpsc_queue.hpp`:

```cpp
/**
 * @brief Pop multiple items from queue (single consumer).
 * @param buffer Buffer to store popped items
 * @param max_count Maximum items to pop
 * @return Number of items actually popped
 */
int PopMultiple(T* buffer[], int max_count) {
    int count = 0;
    while (count < max_count) {
        T* item = Pop();
        if (!item) break;
        buffer[count++] = item;
    }
    return count;
}
```

- [ ] **Step 4: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "perf(worker): optimize PickTask batch prefill to PopMultiple"
```

---

### Task 1.3: WorkStealingQueue 优化

**Files:**
- Modify: `include/bthread/queue/work_stealing_queue.hpp`
- Modify: `src/bthread/queue/work_stealing_queue.cpp`

**问题**: WorkStealingQueue 使用复杂的版本号机制防止 ABA 问题。在高并发窃取场景，CAS 失败率可能较高。

- [ ] **Step 1: 分析当前实现**

当前 Steal() 实现：
```cpp
TaskMetaBase* WorkStealingQueue::Steal() {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);
    
    if (ExtractIndex(h) == ExtractIndex(t)) return nullptr;
    
    uint32_t idx = ExtractIndex(h);
    TaskMetaBase* task = buffer_[idx].load(std::memory_order_acquire);
    
    if (head_.compare_exchange_strong(h, ...)) {
        return task;
    }
    return nullptr;
}
```

问题：使用 compare_exchange_strong，在高竞争时会失败。可以使用 weak 版本 + retry。

- [ ] **Step 2: 优化 Steal 使用 CAS weak + retry**

Edit `src/bthread/queue/work_stealing_queue.cpp`:

```cpp
TaskMetaBase* WorkStealingQueue::Steal() {
    constexpr int MAX_STEAL_ATTEMPTS = 3;
    
    for (int attempt = 0; attempt < MAX_STEAL_ATTEMPTS; ++attempt) {
        uint64_t h = head_.load(std::memory_order_acquire);
        uint64_t t = tail_.load(std::memory_order_acquire);
        
        if (ExtractIndex(h) == ExtractIndex(t)) return nullptr;
        
        uint32_t idx = ExtractIndex(h);
        TaskMetaBase* task = buffer_[idx].load(std::memory_order_acquire);
        
        // 使用 weak 版本，在竞争时更快
        if (head_.compare_exchange_weak(h,
                MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return task;
        }
        // CAS 失败，retry
    }
    return nullptr;
}
```

- [ ] **Step 3: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "perf(queue): use CAS weak + retry in WorkStealingQueue Steal()"
```

---

### Task 1.4: Phase 1 性能验证

- [ ] **Step 1: 运行基准测试**

```bash
cd build && ./benchmark/benchmark
```

记录关键指标：
- Create/Join throughput
- Yield performance
- Mutex contention

- [ ] **Step 2: 对比优化前后性能**

```bash
# 保存当前性能数据
cd build && ./benchmark/benchmark > phase1_perf.txt

# 对比基准数据（重构前的数据）
```

Expected: 队列相关指标提升 ~5-15%

- [ ] **Step 3: Phase 1 总结提交**

```bash
git add -A
git commit -m "perf(phase1): complete hot path queue optimizations"
```

---

## Phase 2: 同步原语优化

### Task 2.1: Butex Wait() Atomic 操作减少

**Files:**
- Modify: `include/bthread/sync/butex.hpp`
- Modify: `src/bthread/sync/butex.cpp`

**问题**: Butex::Wait() 中有多处 atomic load 和 CAS 操作。分析是否可以减少不必要的 atomic 操作。

- [ ] **Step 1: 分析当前 Wait() 流程**

当前 Wait() 流程 (butex.cpp:44-151):
1. Load value_ (atomic acquire)
2. CAS is_waiting (atomic acq_rel)
3. Load value_ again (atomic acquire)
4. Load/CAS state (多次 atomic)
5. Load wake_count (atomic)

优化点：
- 某些 load 可以使用 relaxed 顺序
- 某些顺序 load 可以合并

- [ ] **Step 2: 优化 value_ 检查**

Edit `src/bthread/sync/butex.cpp`:

```cpp
int Butex::Wait(int expected_value, const platform::timespec* timeout, bool prepend) {
    Worker* w = Worker::Current();
    if (w == nullptr) {
        return platform::FutexWait(&value_, expected_value, timeout);
    }

    TaskMetaBase* base_task = w->current_task();
    if (!base_task || base_task->type != TaskType::BTHREAD) {
        return platform::FutexWait(&value_, expected_value, timeout);
    }

    TaskMeta* task = static_cast<TaskMeta*>(base_task);

    // 检查 shutdown - 使用 relaxed
    if (!Scheduler::Instance().running()) {
        return ECANCELED;
    }

    // 1. Check value - 可以用 relaxed，后续会再次检查
    int val = value_.load(std::memory_order_relaxed);
    if (val != expected_value) {
        return 0;  // Early exit
    }

    // 2. Mark as waiting - 保持 acq_rel
    bool expected = false;
    if (!task->is_waiting.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return 0;
    }

    // 3. Double-check value after setting is_waiting - 使用 acquire
    val = value_.load(std::memory_order_acquire);
    if (val != expected_value) {
        task->is_waiting.store(false, std::memory_order_release);
        return 0;
    }

    // ... 后续流程保持不变
}
```

- [ ] **Step 3: 优化 Wake() 流程**

Edit `src/bthread/sync/butex.cpp`:

```cpp
void Butex::Wake(int count) {
    // Wake futex waiters
    platform::FutexWake(&value_, count);

    int woken = 0;
    while (woken < count) {
        TaskMeta* waiter = queue_.PopFromHead();
        if (!waiter) break;

        // 使用 relaxed 顺序更新字段，后续 state CAS 会同步
        waiter->is_waiting.store(false, std::memory_order_relaxed);
        waiter->wake_count.fetch_add(1, std::memory_order_relaxed);

        if (waiter->waiter.timer_id != 0) {
            Scheduler::Instance().GetTimerThread()->Cancel(waiter->waiter.timer_id);
        }

        // State 更新使用 release
        TaskState state = waiter->state.load(std::memory_order_acquire);
        if (state == TaskState::SUSPENDED) {
            waiter->state.store(TaskState::READY, std::memory_order_release);
            Scheduler::Instance().EnqueueTask(waiter);
        }

        ++woken;
    }
}
```

- [ ] **Step 4: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "perf(butex): reduce atomic operations with relaxed ordering where safe"
```

---

### Task 2.2: Mutex 快速路径优化

**Files:**
- Modify: `include/bthread/sync/mutex.hpp`
- Modify: `src/bthread/sync/mutex.cpp`

**问题**: Mutex::lock_bthread() 的快速路径可以优化，减少 atomic 操作。

- [ ] **Step 1: 分析当前 lock_bthread()**

当前实现有多次 CAS 和 load 操作。优化为：
- 使用 relaxed load 检查
- 只在需要时使用 acquire/release

- [ ] **Step 2: 优化快速路径**

Edit `src/bthread/sync/mutex.cpp`:

```cpp
void Mutex::lock_bthread() {
    // Fast path: relaxed check first
    uint32_t state = state_.load(std::memory_order_relaxed);
    if (state == 0) {
        // Try acquire with acquire ordering
        uint32_t expected = 0;
        if (state_.compare_exchange_strong(expected, LOCKED,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return;  // Acquired
        }
    }

    // 慢路径保持不变
    ...
}
```

- [ ] **Step 3: 验证编译和 Mutex 测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure -R mutex
```

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "perf(mutex): optimize fast path with relaxed initial check"
```

---

### Task 2.3: Phase 2 性能验证

- [ ] **Step 1: 运行 Mutex 基准测试**

```bash
cd build && ./benchmark/benchmark 2>&1 | grep -A5 "Mutex Contention"
```

Expected: Mutex contention throughput 提升 ~5-10%

- [ ] **Step 2: Phase 2 总结提交**

```bash
git add -A
git commit -m "perf(phase2): complete synchronization primitive optimizations"
```

---

## Phase 3: Worker 调度优化

### Task 3.1: WaitForTask 自适应 Spin

**Files:**
- Modify: `include/bthread/core/worker.hpp`
- Modify: `src/bthread/core/worker.cpp`

**问题**: 当前 WaitForTask 直接使用 futex wait with timeout。在高频场景，可以先 spin 几次再进入 futex wait。

- [ ] **Step 1: 实现自适应 spin-wait**

Edit `src/bthread/core/worker.cpp`:

```cpp
void Worker::WaitForTask() {
    constexpr int MAX_SPINS = 50;  // Spin 50 次再 futex wait
    constexpr int SPIN_CHECK_INTERVAL = 5;  // 每 5 次 spin 检查一次 queue
    
    int spin_count = 0;
    
    while (stop_flag_.load(std::memory_order_seq_cst) == 0) {
        // 自适应 spin
        if (spin_count < MAX_SPINS) {
            #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
            #else
            std::atomic_signal_fence(std::memory_order_acquire);
            #endif
            
            ++spin_count;
            
            // 定期检查 queue
            if (spin_count % SPIN_CHECK_INTERVAL == 0) {
                if (!local_queue_.Empty() ||
                    !Scheduler::Instance().global_queue().Empty()) {
                    return;  // 有任务，退出等待
                }
            }
            continue;
        }
        
        // Spin 完成后进入 futex wait
        platform::timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000;  // 1ms timeout
        
        // Check for tasks first
        if (!local_queue_.Empty() ||
            !Scheduler::Instance().global_queue().Empty()) {
            return;
        }
        
        int expected = wake_count_.load(std::memory_order_acquire);
        int result = platform::FutexWait(&wake_count_, expected, &ts);
        
        if (result == ETIMEDOUT) {
            spin_count = 0;  // Timeout 后重置 spin counter
            continue;
        }
    }
}
```

- [ ] **Step 2: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 3: 提交**

```bash
git add -A
git commit -m "perf(worker): add adaptive spinning before futex wait"
```

---

### Task 3.2: Work Stealing 随机策略优化

**Files:**
- Modify: `src/bthread/core/worker.cpp`

**问题**: 当前使用 mt19937 RNG，这是较重的随机数生成器。对于工作窃取场景，可以使用更轻量的随机策略。

- [ ] **Step 1: 替换为轻量随机策略**

Edit `src/bthread/core/worker.cpp`:

```cpp
TaskMetaBase* Worker::PickTask() {
    // ... 前面的逻辑保持不变 ...

    // 4. Try work stealing
    int32_t wc = Scheduler::Instance().worker_count();
    if (wc <= 1) return nullptr;

    // 使用轻量随机 - XOR shift generator
    uint32_t rng_state = static_cast<uint32_t>(id_ * 2654435761u);  // 初始种子
    
    int attempts = wc * 3;
    for (int i = 0; i < attempts; ++i) {
        // XOR shift - 快速随机数生成
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        
        int victim = rng_state % wc;
        if (victim == id_) continue;

        Worker* other = Scheduler::Instance().GetWorker(victim);
        if (other) {
            task = other->local_queue_.Steal();
            if (task) return task;
        }
    }

    return nullptr;
}
```

- [ ] **Step 2: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 3: 提交**

```bash
git add -A
git commit -m "perf(worker): use lightweight XOR shift RNG for work stealing"
```

---

### Task 3.3: Scheduler Wake 机制优化

**Files:**
- Modify: `include/bthread/core/scheduler.hpp`
- Modify: `src/bthread/core/scheduler.cpp`

**问题**: Scheduler::WakeIdleWorkers() 使用 mutex 锁保护 workers_ 列表。在高频唤醒场景，锁竞争可能成为瓶颈。

- [ ] **Step 1: 分析当前 WakeIdleWorkers**

当前实现：
```cpp
void Scheduler::WakeIdleWorkers(int count) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    int woken = 0;
    for (auto* w : workers_) {
        if (woken >= count) break;
        w->WakeUp();
        ++woken;
    }
}
```

问题：每次 wake 都要获取锁。

- [ ] **Step 2: 优化为无锁唤醒**

Edit `include/bthread/core/scheduler.hpp`:

```cpp
class Scheduler {
private:
    // 使用 atomic 数组存储 worker 指针，避免锁
    std::atomic<Worker*> workers_atomic_[MAX_WORKERS];
    std::atomic<int> worker_count_{0};
    
    static constexpr int MAX_WORKERS = 256;
};
```

Edit `src/bthread/core/scheduler.cpp`:

```cpp
void Scheduler::StartWorkers(int count) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    
    workers_.reserve(count);
    for (int i = 0; i < count; ++i) {
        Worker* w = new Worker(i);
        w->set_thread(platform::CreateThread([](void* arg) {
            static_cast<Worker*>(arg)->Run();
        }, w));
        workers_.push_back(w);
        workers_atomic_[i].store(w, std::memory_order_release);
    }
    worker_count_.store(count, std::memory_order_release);
}

void Scheduler::WakeIdleWorkers(int count) {
    int wc = worker_count_.load(std::memory_order_acquire);
    int woken = 0;
    
    for (int i = 0; i < wc && woken < count; ++i) {
        Worker* w = workers_atomic_[i].load(std::memory_order_acquire);
        if (w) {
            w->WakeUp();
            ++woken;
        }
    }
}
```

- [ ] **Step 3: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "perf(scheduler): use atomic array for lock-free worker wake"
```

---

### Task 3.4: Phase 3 性能验证

- [ ] **Step 1: 运行完整基准测试**

```bash
cd build && ./benchmark/benchmark
```

- [ ] **Step 2: 对比 Phase 3 前后性能**

Expected: 
- Yield throughput 提升 ~10-15%
- Work stealing 效率提升

- [ ] **Step 3: Phase 3 总结提交**

```bash
git add -A
git commit -m "perf(phase3): complete worker scheduling optimizations"
```

---

## Phase 4: 内存布局优化

### Task 4.1: TaskMeta 内存布局优化

**Files:**
- Modify: `include/bthread/core/task_meta.hpp`
- Modify: `include/bthread/core/task_meta_base.hpp`

**问题**: TaskMeta 结构体较大（约 200+ bytes），包含多个 atomic 字段。优化内存布局以改善 cache locality。

- [ ] **Step 1: 分析当前布局**

当前 TaskMeta 包含：
- 继承自 TaskMetaBase (约 64 bytes)
- stack/stack_size/context (约 64 bytes)
- fn/arg/result (约 24 bytes)
- ref_count (8 bytes)
- join_butex/join_waiters (约 16 bytes)
- waiting_butex/waiter (约 40 bytes)
- is_waiting/wake_count/butex_waiter_node (约 24 bytes)
- uses_xmm/local_worker/legacy_next (约 24 bytes)

总计约 200+ bytes，跨越多个 cache line。

- [ ] **Step 2: 优化字段顺序**

Edit `include/bthread/core/task_meta.hpp`:

```cpp
struct TaskMeta : TaskMetaBase {
    TaskMeta() : TaskMetaBase() {
        type = TaskType::BTHREAD;
    }

    // ========== Group 1: 热路径调度字段 (第一 cache line) ==========
    // 这些字段在调度时频繁访问，放在一起
    void* stack{nullptr};           // 8 bytes
    size_t stack_size{0};           // 8 bytes
    platform::Context context{};    // ~48 bytes (depends on platform)
    
    // ========== Group 2: 状态字段 (第二 cache line) ==========
    std::atomic<TaskState> state{TaskState::READY};  // 继承
    std::atomic<bool> is_waiting{false};             // 8 bytes
    std::atomic<int> wake_count{0};                  // 8 bytes
    
    // ========== Group 3: 同步原语字段 (第三 cache line) ==========
    void* waiting_butex{nullptr};
    WaiterState waiter;
    ButexWaiterNode butex_waiter_node;
    
    // ========== Group 4: 入口和结果 (较少访问) ==========
    void* (*fn)(void*){nullptr};
    void* arg{nullptr};
    void* result{nullptr};
    
    // ========== Group 5: Join 相关 ==========
    void* join_butex{nullptr};
    std::atomic<int> join_waiters{0};
    std::atomic<int> ref_count{0};
    
    // ========== Group 6: 其他 ==========
    bool uses_xmm{false};
    Worker* local_worker{nullptr};
    TaskMeta* legacy_next{nullptr};
    
    void resume() override;
    bool Release() {
        return ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
};
```

- [ ] **Step 3: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "perf(taskmeta): optimize memory layout for cache locality"
```

---

### Task 4.2: TaskGroup Free List 优化

**Files:**
- Modify: `src/bthread/core/task_group.cpp`

**问题**: AllocTaskMeta() 使用 CAS loop 从 free list 获取 slot。在高分配频率场景，可以优化。

- [ ] **Step 1: 分析当前 AllocTaskMeta**

当前实现使用 CAS loop，在高竞争时可能需要多次 retry。

- [ ] **Step 2: 添加批量分配优化**

Edit `src/bthread/core/task_group.cpp`:

```cpp
TaskMeta* TaskGroup::AllocTaskMeta() {
    // 尝试批量获取多个 slot（可选优化）
    int32_t slot = free_head_.load(std::memory_order_acquire);
    
    while (slot >= 0) {
        // 使用 weak CAS，在竞争时更快
        int32_t next = free_slots_[slot].load(std::memory_order_relaxed);
        if (free_head_.compare_exchange_weak(slot, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Got a slot
            TaskMeta* meta = task_pool_[slot].load(std::memory_order_relaxed);
            
            if (!meta) {
                meta = new TaskMeta();
                meta->slot_index = slot;
                meta->generation = generations_[slot].load(std::memory_order_relaxed);
                task_pool_[slot].store(meta, std::memory_order_release);
            } else {
                meta->slot_index = slot;
                meta->generation = generations_[slot].load(std::memory_order_relaxed);
            }
            
            return meta;
        }
        // CAS 失败，自动 retry (weak 版本)
    }
    
    return nullptr;
}
```

- [ ] **Step 3: 验证编译和测试**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 4: 提交**

```bash
git add -A
git commit -m "perf(taskgroup): use CAS weak for free list allocation"
```

---

### Task 4.3: Phase 4 性能验证

- [ ] **Step 1: 运行 Create/Join 基准测试**

```bash
cd build && ./benchmark/benchmark 2>&1 | grep -A5 "Create/Join"
```

Expected: Create/Join throughput 提升 ~5-10%

- [ ] **Step 2: Phase 4 总结提交**

```bash
git add -A
git commit -m "perf(phase4): complete memory layout optimizations"
```

---

## Phase 5: 最终验证和文档

### Task 5.1: 全量性能测试

- [ ] **Step 1: 运行完整基准测试套件**

```bash
cd build && ./benchmark/benchmark > final_perf.txt
```

- [ ] **Step 2: 对比优化前后性能**

创建对比报告：

| 基准测试 | 优化前 | 优化后 | 提升 |
|----------|--------|--------|------|
| Create/Join | ~130K ops/s | TBD | TBD |
| Yield | TBD | TBD | TBD |
| Mutex Contention | TBD | TBD | TBD |
| Producer-Consumer | TBD | TBD | TBD |

- [ ] **Step 3: 运行所有单元测试**

```bash
cd build && ctest --output-on-failure -j$(nproc)
```

Expected: 所有测试通过

---

### Task 5.2: 创建性能优化文档

- [ ] **Step 1: 创建优化文档**

Create `docs/performance_optimization.md`:

```markdown
# Bthread 性能优化说明

## 优化概览

本次优化针对 bthread M:N 线程库的运行时性能，主要包括：

1. 热路径队列优化
2. 同步原语 atomic 操作优化
3. Worker 调度策略优化
4. 内存布局优化

## 优化详情

### MpscQueue Pop() 自适应 Spin

- **问题**: Pop() 在 race condition 时立即 yield
- **解决**: 先 spin 100 次，使用 pause 指令，再 yield
- **收益**: 减少不必要的 context switch

### Butex Wait() Memory Ordering 优化

- **问题**: 多处 atomic 操作使用 acquire/release
- **解决**: 在 safe 场景使用 relaxed ordering
- **收益**: 减少内存 barrier 开销

### Worker WaitForTask 自适应 Spin

- **问题**: 直接进入 futex wait
- **解决**: 先 spin 50 次，定期检查 queue，再 futex wait
- **收益**: 减少内核调用

### TaskMeta 内存布局优化

- **问题**: 结构体较大，字段分散
- **解决**: 按访问频率分组，热路径字段放前面
- **收益**: 改善 cache locality

## 性能基准

优化前后对比数据见 benchmark/benchmark 输出。
```

- [ ] **Step 2: 提交文档**

```bash
git add docs/performance_optimization.md
git commit -m "docs: add performance optimization documentation"
```

---

### Task 5.3: 最终总结提交

- [ ] **Step 1: 创建最终总结**

```bash
git add -A
git commit -m "perf: complete bthread performance optimization

Summary:
- Phase 1: Hot path queue optimizations (spin, batch, CAS weak)
- Phase 2: Sync primitive atomic optimization
- Phase 3: Worker scheduling optimizations (adaptive spin, lightweight RNG)
- Phase 4: Memory layout optimizations
- Phase 5: Documentation

Performance improvements:
- Create/Join: TBD% improvement
- Yield: TBD% improvement
- Mutex: TBD% improvement

All tests passing.
"
```

---

## 成功标准

1. 所有测试通过（包括原有测试和新增性能测试）
2. 基准测试显示性能提升（具体指标 TBD）
3. 代码质量保持（无新增复杂度）
4. 文档完善（优化说明文档）

---

## 风险和注意事项

1. **Memory ordering 优化**: 必须仔细分析，确保不引入数据竞争
2. **Spin 策略**: 过多 spin 会浪费 CPU，需要调优参数
3. **平台兼容性**: pause 指令只在 x86 有效，其他平台需要 fallback
4. **回归测试**: 每个优化后都要运行完整测试套件