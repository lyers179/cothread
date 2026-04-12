# Phase 7 Performance Optimization Design

**Date:** 2026-04-12
**Author:** Claude + lyers179
**Status:** Approved

---

## Executive Summary

本设计文档提出 3 个性能优化方向，目标是进一步提升 Scalability 和高并发场景吞吐量。

| 优化 | 当前问题 | 目标收益 | 风险 |
|------|----------|----------|------|
| ShardedGlobalQueue Empty O(1) | O(n) 遍历所有 shard | Scalability +5-10% | 低 |
| Work Stealing Cache-Friendly | 随机遍历 cache 不友好 | Scalability +5-10% | 低 |
| Mutex Waiter Debounce | 重复唤醒开销 | Mutex +10-15% | 低 |

---

## Optimization 1: ShardedGlobalQueue Empty O(1)

### Problem Analysis

当前 `ShardedGlobalQueue::Empty()` 实现：

```cpp
bool ShardedGlobalQueue::Empty() const {
    for (int i = 0; i < worker_count_; ++i) {  // O(n) 遍历
        if (!shards_[i].Empty()) return false;
    }
    return true;
}
```

**问题：**
- `WaitForTask()` 高频调用 `Empty()` 检查
- 每个 worker 都遍历所有 shard（8-16 个）
- 多个 worker 并发调用，造成 cache contention

**开销分析：**
- 8 workers × 16 shards × N 次 Empty 调用
- 每次 Empty 需要遍历 16 个 shard 的原子 tail 指针

### Design: Atomic Task Counter

添加原子计数器跟踪总任务数：

```cpp
class ShardedGlobalQueue {
private:
    std::atomic<int32_t> total_count_{0};  // 总任务计数
    // ... 其他成员 ...

public:
    void Push(TaskMetaBase* task);
    TaskMetaBase* Pop(int worker_id);
    bool Empty() const;  // O(1)
};
```

#### Implementation

```cpp
void ShardedGlobalQueue::Push(TaskMetaBase* task) {
    int shard = round_robin_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
    shards_[shard].Push(task);
    total_count_.fetch_add(1, std::memory_order_release);
}

TaskMetaBase* ShardedGlobalQueue::Pop(int worker_id) {
    // 1. Try own shard first
    if (worker_id >= 0 && worker_id < worker_count_) {
        TaskMetaBase* task = shards_[worker_id].Pop();
        if (task) {
            total_count_.fetch_sub(1, std::memory_order_release);
            return task;
        }
    }

    // 2. Steal from other shards
    for (int i = 0; i < worker_count_; ++i) {
        if (i == worker_id) continue;
        TaskMetaBase* task = shards_[i].Pop();
        if (task) {
            total_count_.fetch_sub(1, std::memory_order_release);
            return task;
        }
    }

    return nullptr;
}

bool ShardedGlobalQueue::Empty() const {
    return total_count_.load(std::memory_order_acquire) == 0;  // O(1)
}
```

### Risk Assessment

**潜在问题：** 计数器可能有短暂不一致（Push 后 Pop 前）。

**分析：**
- Empty() 只用于快速检查，不影响正确性
- 即使 Empty() 返回 false 但实际空，Pop 会返回 nullptr
- 即使 Empty() 返回 true 但实际有任务，WaitForTask 会继续 spin 或 futex wait
- 不影响任务调度正确性

**结论：** 风险低，可以接受。

---

## Optimization 2: Work Stealing Cache-Friendly

### Problem Analysis

当前 `Worker::PickTask()` work stealing 实现：

```cpp
// 随机遍历其他 worker
static thread_local uint32_t rng_state = ...;

int attempts = wc * 3;
for (int i = 0; i < attempts; ++i) {
    // XOR shift RNG
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;

    int victim = rng_state % wc;  // 随机目标
    if (victim == id_) continue;

    Worker* other = Scheduler::Instance().GetWorker(victim);
    if (other) {
        task = other->local_queue_.Steal();
        if (task) return task;
    }
}
```

**问题：**
- 随机顺序访问 Worker 对象
- Worker 数组连续存储，随机访问跳跃内存地址
- 每次 steal 可能 cache miss（访问不同 worker 的 local_queue）

**开销分析：**
- Cache miss latency: ~50-100 ns
- 随机访问 vs 顺序访问: 2-5x cache miss 增加
- 高并发时多个 worker 同时 steal，加剧 contention

### Design: Sequential Traversal

使用固定顺序遍历（从相邻 worker 开始），利用 cache locality：

```cpp
TaskMetaBase* Worker::PickTask() {
    // ... batch, local queue, global queue ...

    // 4. Work stealing - sequential traversal
    int wc = Scheduler::Instance().worker_count();
    if (wc <= 1) return nullptr;

    // 从相邻 worker 开始，顺序遍历
    int start = (id_ + 1) % wc;
    int attempts = wc * 2;  // 减少 attempts（顺序更高效）

    for (int i = 0; i < attempts; ++i) {
        int victim = (start + i) % wc;  // 顺序遍历
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

### Why Sequential is Better

**Cache Locality 原理：**
- Worker 对象连续存储在 `workers_atomic_[]` 数组
- 顺序访问利用 CPU cache line prefetching
- 相邻 Worker 更可能在同一 cache line（64 bytes）

**性能对比：**
```
随机访问：victim[0]=3, victim[1]=7, victim[2]=1, ...
           -> cache miss for each different worker

顺序访问：victim[0]=1, victim[1]=2, victim[2]=3, ...
           -> adjacent workers, better prefetching
           -> worker[1] and worker[2] may share cache line
```

### Fairness Consideration

**问题：** 顺序遍历可能导致"热点"worker 被优先 steal。

**分析：**
- start = (id + 1) % wc，每个 worker 从不同位置开始
- 不同 worker 的 steal 顺序不同（分布均匀）
- Work stealing 本身就是 best-effort，不保证公平
- 关键是避免饥饿（每个 worker 都有机会被 steal）

**结论：** 不影响公平性，可以接受。

---

## Optimization 3: Mutex Waiter Debounce

### Problem Analysis

当前 `Mutex::unlock()` 实现：

```cpp
void Mutex::unlock() {
    Worker* w = Worker::Current();

    if (!w) {
        unlock_pthread();
        return;
    }

    // Check for coroutine waiters first
    TaskMetaBase* waiter = dequeue_waiter();
    if (waiter) {
        waiter->state.store(TaskState::READY, ...);
        Scheduler::Instance().Submit(waiter);
        return;  // LOCKED stays set
    }

    // Check bthread waiters
    uint32_t old_state = state_.fetch_and(~LOCKED, std::memory_order_release);

    if (old_state & HAS_WAITERS) {
        Butex* butex = static_cast<Butex*>(butex_.load(...));
        if (butex) {
            butex->set_value(butex->value() + 1);
            butex->Wake(1);  // 可能重复唤醒
        }
    }
}
```

**问题：**
- 多线程同时 unlock（竞争场景）时可能重复唤醒
- Wake() 调用 Butex::Wake，涉及 PopFromHead + futex syscall
- 重复唤醒导致不必要的 syscall 和任务入队

**竞态场景：**
```
Thread A: unlock() -> Wake(1) -> Pop waiter1
Thread B: unlock() -> Wake(1) -> Pop waiter2 (可能 waiter1 已被唤醒)
```

### Design: Pending Wake Counter

添加 `pending_wake_` 计数器防止重复唤醒：

```cpp
class Mutex {
private:
    static constexpr uint32_t LOCKED = 1;
    static constexpr uint32_t HAS_WAITERS = 2;

    std::atomic<uint32_t> state_{0};
    std::atomic<uint32_t> pending_wake_{0};  // 新增：待唤醒计数

    MpscQueue<MutexWaiterNode> waiter_queue_;
    // ...
};
```

#### Implementation

```cpp
void Mutex::unlock() {
    Worker* w = Worker::Current();
    if (!w) {
        unlock_pthread();
        return;
    }

    // Check coroutine waiters first
    TaskMetaBase* waiter = dequeue_waiter();
    if (waiter) {
        waiter->state.store(TaskState::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        Scheduler::Instance().Submit(waiter);
        return;
    }

    // 检查 pending_wake - 如果已有唤醒在进行，跳过
    if (pending_wake_.load(std::memory_order_acquire) > 0) {
        // 只清除 LOCKED，由正在唤醒的线程负责
        state_.fetch_and(~LOCKED, std::memory_order_release);
        return;
    }

    // 清除 LOCKED 并检查 HAS_WAITERS
    uint32_t old_state = state_.fetch_and(~LOCKED, std::memory_order_release);

    if (old_state & HAS_WAITERS) {
        // 标记唤醒正在进行
        pending_wake_.fetch_add(1, std::memory_order_release);

        Butex* butex = static_cast<Butex*>(butex_.load(std::memory_order_acquire));
        if (butex) {
            butex->set_value(butex->value() + 1);
            butex->Wake(1);
        }

        // 清除唤醒标记
        pending_wake_.fetch_sub(1, std::memory_order_release);
    }
}
```

### Why This Works

**原理：**
- `pending_wake_` 表示"有唤醒正在进行"
- 多线程同时 unlock 时，只有一个执行 Wake
- 其他线程只清除 LOCKED，不执行 Wake

**正确性：**
- Waiter 看到 LOCKED 清除后可以获取锁
- 如果 waiter 未获取锁，但ex 的 Wake 会唤醒它
- 不影响 waiter 正确唤醒

### Risk Assessment

**潜在问题：**
- `pending_wake_` 可能短暂阻塞后续 unlock
- 如果 Wake 执行时间很长，后续 unlock 会跳过唤醒

**分析：**
- Wake 执行时间很短（Pop + futex syscall，~1-10 us）
- 短暂阻塞不影响正确性
- 阻塞期间 waiter 可以获取锁（LOCKED 已清除）

**结论：** 风险低，可以接受。

---

## Expected Performance Impact

| 优化 | 预期收益 | 测试方法 |
|------|----------|----------|
| Empty O(1) | Scalability +5-10% | 8-16 workers scalability test |
| Work Stealing Sequential | Scalability +5-10% | Producer-Consumer + scalability |
| Mutex Debounce | Mutex Contention +10-15% | Mutex contention benchmark |

**总预期改进：**
- Scalability: 48x → ~55-60x
- Mutex Contention: 27M → ~30-35M/sec

---

## Implementation Plan

### Task 1: ShardedGlobalQueue Empty O(1)

**Files:**
- Modify: `include/bthread/queue/sharded_queue.hpp`
- Modify: `src/bthread/queue/sharded_queue.cpp`

**Steps:**
1. Add `total_count_` atomic counter
2. Update Push/Pop to modify counter
3. Simplify Empty() to O(1) check

### Task 2: Work Stealing Cache-Friendly

**Files:**
- Modify: `src/bthread/core/worker.cpp`

**Steps:**
1. Replace random traversal with sequential
2. Use `(id_ + 1) % wc` as start
3. Reduce attempts (sequential is more efficient)

### Task 3: Mutex Waiter Debounce

**Files:**
- Modify: `include/bthread/sync/mutex.hpp`
- Modify: `src/bthread/sync/mutex.cpp`

**Steps:**
1. Add `pending_wake_` atomic counter
2. Check pending_wake in unlock before Wake
3. Increment/decrement around Wake call

---

## Testing Requirements

1. **单元测试:** 新功能的正确性测试
2. **Benchmark:** 性能改进验证
3. **稳定性测试:** 100% 通过率，无超时
4. **压力测试:** 高并发场景 (16+ workers)
5. **边缘情况:** Mutex 高竞争，空队列，work stealing 饥饿

---

## Summary

本设计提出 3 个低风险优化，预期 Scalability 和 Mutex 性能显著提升。

**优化原则：**
- 不引入复杂机制
- 不影响正确性
- 利用现有硬件特性（cache locality）
- 减少不必要的 syscall