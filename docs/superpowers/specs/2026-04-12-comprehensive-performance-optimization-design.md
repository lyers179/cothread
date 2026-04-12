# Comprehensive Performance Optimization Design

**Date:** 2026-04-12
**Author:** Claude + lyers179
**Status:** Draft

---

## Executive Summary

本设计文档提出 5 个性能优化方向，目标是恢复 Phase 3 的性能水平并进一步提升。

| 优化 | 当前性能 | 目标性能 | 预期改进 |
|------|----------|----------|----------|
| WakeIdleWorkers | ~150K ops/sec | ~170-180K | +15-20% |
| Mutex Lock-Free | 12M/sec | ~18M/sec | +50% |
| Global Queue MPMC | 5.6x scalability | ~7x | +25% |
| Timer Sharding | Baseline | +10-20% | Lower contention |
| Yield Optimization | 8M/sec | ~15-20M/sec | +100-150% |

---

## Optimization 1: WakeIdleWorkers - Selective Wake

### Problem Analysis

当前 `WakeIdleWorkers(1)` 唤醒 **所有** worker：

```cpp
// scheduler.cpp:253-269
void Scheduler::WakeIdleWorkers(int count) {
    int wc = worker_count_.load();
    for (int i = 0; i < wc; ++i) {  // 唤醒全部 8+ workers
        w->WakeUp();                  // 每次: atomic increment + futex syscall
    }
}
```

**开销分析:**
- 8 workers × (atomic increment + potential futex syscall)
- 大多数 worker 会发现任务已被其他 worker 取走
- 不必要的 CPU 噪声

### Design: Idle Worker Registry

使用原子链表维护真正空闲的 worker，只唤醒链表中的 N 个。

#### Data Structure

```cpp
class Scheduler {
private:
    // 空闲 worker 注册表 (lock-free linked list)
    std::atomic<int> idle_head_{-1};      // 链表头
    std::atomic<int> idle_next_[MAX_WORKERS];  // 每个节点指向下一个

public:
    void RegisterIdle(int worker_id);
    int PopIdleWorker();   // 返回 -1 表示无空闲 worker
    void PushIdleWorker(int worker_id);  // 放回空闲列表
};
```

#### Implementation

**RegisterIdle (Worker 调用):**

```cpp
void Scheduler::RegisterIdle(int worker_id) {
    // 将自己加入空闲列表
    int old_head = idle_head_.load(std::memory_order_acquire);
    do {
        idle_next_[worker_id].store(old_head, std::memory_order_relaxed);
    } while (!idle_head_.compare_exchange_weak(old_head, worker_id,
            std::memory_order_release, std::memory_order_acquire));
}
```

**PopIdleWorker (Scheduler 调用):**

```cpp
int Scheduler::PopIdleWorker() {
    int worker_id = idle_head_.load(std::memory_order_acquire);
    while (worker_id >= 0) {
        int next = idle_next_[worker_id].load(std::memory_order_relaxed);
        if (idle_head_.compare_exchange_weak(worker_id, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return worker_id;  // 成功取出一个空闲 worker
        }
        // CAS 失败，重试
    }
    return -1;  // 无空闲 worker
}
```

**WakeIdleWorkers (优化后):**

```cpp
void Scheduler::WakeIdleWorkers(int count) {
    for (int i = 0; i < count; ++i) {
        int idle_id = PopIdleWorker();
        if (idle_id < 0) break;  // 无空闲 worker
        Worker* w = workers_atomic_[idle_id].load(std::memory_order_acquire);
        if (w) w->WakeUp();
    }
}
```

**Worker::WaitForTask 修改:**

```cpp
void Worker::WaitForTask() {
    // ... spin phase ...
    
    is_idle_.store(true, std::memory_order_seq_cst);
    Scheduler::Instance().RegisterIdle(id_);  // 注册空闲
    
    // ... futex wait ...
    
    is_idle_.store(false, std::memory_order_release);
    // 从空闲列表移除 (WakeUp 后自动移除，或超时后移除)
}
```

### Risk Mitigation

1. **保留 is_idle_ 检查:** 空闲列表是建议性的，WakeUp 仍检查 is_idle_
2. **超时移除:** Worker futex 超时后从列表移除自己
3. **Shutdown 清理:** Stop() 清空空闲列表

### Expected Impact

- Create/Join: +15-20%
- Scalability: 更好 (减少唤醒噪声)
- Futex syscall 减少: O(N) → O(min(N, idle))

---

## Optimization 2: Mutex Waiter Queue Lock-Free

### Problem Analysis

当前 Mutex 的 waiter 队列使用 `std::mutex`:

```cpp
void Mutex::enqueue_waiter(TaskMetaBase* task) {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    MutexWaiterNode* node = new MutexWaiterNode{task, nullptr};
    // ... linked list operations ...
}
```

**开销:** 每次 enqueue/dequeue 都需要 mutex lock。

### Design: Replace with MpscQueue

使用已有的 `MpscQueue<T>` (lock-free MPSC):

#### Data Structure

```cpp
// 修改 MutexWaiterNode 添加 next 指针
struct MutexWaiterNode {
    TaskMetaBase* task;
    std::atomic<MutexWaiterNode*> next{nullptr};  // MpscQueue 需要
};

class Mutex {
private:
    // 替换: std::mutex waiters_mutex_ + linked list
    MpscQueue<MutexWaiterNode> waiter_queue_;

public:
    void enqueue_waiter(TaskMetaBase* task);
    TaskMetaBase* dequeue_waiter();
};
```

#### Implementation

```cpp
void Mutex::enqueue_waiter(TaskMetaBase* task) {
    MutexWaiterNode* node = new MutexWaiterNode{task};
    waiter_queue_.Push(node);  // Lock-free
}

TaskMetaBase* Mutex::dequeue_waiter() {
    MutexWaiterNode* node = waiter_queue_.Pop();  // Lock-free
    if (!node) return nullptr;
    TaskMetaBase* task = node->task;
    delete node;
    return task;
}
```

### Compatibility

- `MpscQueue::Push` 多生产者安全 ✓
- `MpscQueue::Pop` 单消费者 - Mutex unlock 只在一个线程 ✓

### Expected Impact

- Mutex Contention: 12M → ~18M/sec (+50%)
- 简化代码，消除 mutex 竞争

---

## Optimization 3: Global Queue MPMC

### Problem Analysis

当前 `GlobalQueue = MpscQueue` 是 MPSC (单消费者):

```cpp
using GlobalQueue = TaskQueue;  // MpscQueue
```

**问题:** 多个 worker 尝试 Pop 时只有一个成功，其他必须 steal。

### Design: Sharded Global Queue

使用分片策略，每个 worker 有自己的 shard，steal 时从其他 shard 取。

#### Data Structure

```cpp
class ShardedGlobalQueue {
private:
    std::atomic<int> round_robin_{0};
    int worker_count_{0};
    MpscQueue<TaskMetaBase> shards_[MAX_WORKERS];

public:
    void Init(int worker_count);
    void Push(TaskMetaBase* task);
    TaskMetaBase* Pop(int worker_id);
};
```

#### Implementation

```cpp
void ShardedGlobalQueue::Init(int worker_count) {
    worker_count_ = worker_count;
}

void ShardedGlobalQueue::Push(TaskMetaBase* task) {
    // Round-robin 分配到 shards
    int shard = round_robin_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
    shards_[shard].Push(task);
}

TaskMetaBase* ShardedGlobalQueue::Pop(int worker_id) {
    // 先尝试自己的 shard
    TaskMetaBase* task = shards_[worker_id].Pop();
    if (task) return task;
    
    // Steal from 其他 shards
    for (int i = 0; i < worker_count_; ++i) {
        if (i == worker_id) continue;
        task = shards_[i].Pop();
        if (task) return task;
    }
    return nullptr;
}
```

#### Scheduler Integration

```cpp
class Scheduler {
private:
    ShardedGlobalQueue global_queue_;

public:
    void Submit(TaskMetaBase* task) {
        if (current_worker) {
            current_worker->local_queue().Push(task);
            WakeIdleWorkers(1);
        } else {
            global_queue_.Push(task);  // 使用分片队列
            WakeIdleWorkers(1);
        }
    }
    
    TaskMetaBase* PopGlobal(int worker_id) {
        return global_queue_.Pop(worker_id);
    }
};
```

#### Worker Integration

```cpp
TaskMetaBase* Worker::PickTask() {
    // 1. batch
    // 2. local queue
    // 3. global queue (通过 shard)
    TaskMetaBase* task = Scheduler::Instance().PopGlobal(id_);
    if (task) return task;
    // 4. work stealing ...
}
```

### Expected Impact

- Scalability: 5.6x → ~7x (+25%)
- Workers 可并发 Pop global queue

---

## Optimization 4: Timer Thread Sharding

### Problem Analysis

当前 TimerThread 使用单一 mutex 保护 heap:

```cpp
int TimerThread::Schedule(...) {
    std::lock_guard<std::mutex> lock(heap_mutex_);
    AddToHeap(entry);
}
```

**问题:** 所有 timer 操作竞争同一 mutex。

### Design: Per-Worker Timer Shard

#### Data Structure

```cpp
class TimerThread {
private:
    struct TimerShard {
        std::mutex mutex;  // Per-shard lock
        std::vector<TimerEntry*> heap;
        std::atomic<int64_t> next_deadline{INT64_MAX};
    };
    
    TimerShard shards_[MAX_WORKERS];
    std::atomic<int> shard_assign_{0};
    int worker_count_{0};

public:
    void Init(int worker_count);
    int Schedule(void (*callback)(void*), void* arg, timespec* delay);
    void TimerThreadMain();
};
```

#### Implementation

```cpp
void TimerThread::Init(int worker_count) {
    worker_count_ = worker_count;
}

int TimerThread::Schedule(void (*callback)(void*), void* arg, timespec* delay) {
    int shard_id = shard_assign_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
    
    auto* entry = new TimerEntry();
    entry->callback = callback;
    entry->arg = arg;
    entry->deadline_us = GetTimeOfDayUs() + delay_us;
    entry->id = next_id_.fetch_add(1);
    
    auto& shard = shards_[shard_id];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.heap.push_back(entry);
        std::push_heap(shard.heap.begin(), shard.heap.end(),
            [](auto* a, auto* b) { return a->deadline_us > b->deadline_us; });
        shard.next_deadline.store(shard.heap[0]->deadline_us, std::memory_order_release);
    }
    
    // 如果 deadline 更早，唤醒 timer thread
    WakeIfEarlier(entry->deadline_us);
    
    return entry->id;
}

void TimerThread::TimerThreadMain() {
    while (running_) {
        int64_t now = GetTimeOfDayUs();
        int64_t min_deadline = INT64_MAX;
        
        // 处理所有 shard 中超时的 timer
        for (int i = 0; i < worker_count_; ++i) {
            auto& shard = shards_[i];
            if (shard.next_deadline.load(std::memory_order_acquire) <= now) {
                ProcessShard(shard);
            }
            // 更新最小 deadline
            int64_t shard_deadline = shard.next_deadline.load();
            if (shard_deadline < min_deadline) min_deadline = shard_deadline;
        }
        
        // 等待到最小 deadline
        SleepUntil(min_deadline);
    }
}
```

### Expected Impact

- Timer contention 降低
- Timeout-heavy workloads: +10-20%

---

## Optimization 5: Yield Path Optimization

### Problem Analysis

Yield 从 Phase 3 (32M/sec) 回退到 Phase 5+ (8M/sec)。

**可能原因:**
1. Wake-all 模式开销
2. Step 7.5 安全检查
3. MPMC queue 复杂度
4. Context switch assembly 变化

### Design: Fast Path Yield + Profiling

#### Fast Path Optimization

```cpp
int Worker::YieldCurrent() {
    if (!current_task_) return EINVAL;
    
    // Fast path: 无竞争时直接 yield
    // 避免不必要的队列操作和唤醒
    if (batch_count_ == 0 && local_queue_.Empty()) {
        // 直接 swap context，不经过队列
        SuspendCurrent();
        return 0;
    }
    
    // Normal path: batch enqueue
    current_task_->state.store(TaskState::READY, std::memory_order_release);
    local_batch_[batch_count_++] = current_task_;
    MaybeFlushBatch();
    SuspendCurrent();
    return 0;
}
```

#### Profiling Investigation

需要运行 `perf record` 确定热点:

```bash
perf record -g -o perf.data ./benchmark/benchmark
perf report -i perf.data --stdio --no-children | head -50
```

#### Additional Check

确认 Yield 不调用 `WakeIdleWorkers`:

```cpp
// 如果 Yield 当前调用了 WakeIdleWorkers，应该移除
// Yield 后 task 在 local_batch/local_queue，会被 PickTask 取走
// 不需要唤醒其他 worker
```

### Expected Impact

- Yield: 8M → ~15-20M/sec (如果 wake-all 是原因)
- 需要 profiling 确认根因

---

## Implementation Order

| 阶段 | 优化 | 依赖 | 预期影响 |
|------|------|------|----------|
| 1 | WakeIdleWorkers | 无 | +15-20% Create/Join |
| 2 | Mutex Lock-Free | 无 | +50% Mutex Contention |
| 3 | Global Queue MPMC | 需要测试 | +25% Scalability |
| 4 | Timer Sharding | 无 | +10% Timeout workload |
| 5 | Yield Fast Path | 需要 perf 分析 | +100-150% Yield |

---

## Risk Assessment

| 优化 | 风险级别 | 说明 |
|------|----------|------|
| WakeIdleWorkers | Medium | 空闲列表可能有竞态，需要仔细测试 |
| Mutex Lock-Free | Low | 使用已有的 MpscQueue，风险低 |
| Global Queue MPMC | Medium | 分片 steal 逻辑需要验证 |
| Timer Sharding | Low | 分片 mutex，简单改造 |
| Yield Fast Path | Low | 只在无竞争时生效，不影响正常路径 |

---

## Testing Requirements

每个优化需要通过的测试:

1. **单元测试:** 新组件的正确性测试
2. **Benchmark:** 性能改进验证
3. **稳定性测试:** 100% 通过率，无超时
4. **压力测试:** 高并发场景 (100+ threads)
5. **边缘情况:** Shutdown, timeout, cancellation

---

## Summary

本设计提出 5 个优化方向，预期总体性能改进:

- Create/Join: +15-20%
- Mutex Contention: +50%
- Scalability: +25%
- Yield: +100-150% (需确认根因)

总改进预期: 恢复 Phase 3 性能水平并超越。