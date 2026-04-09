# Bthread 性能优化说明

## 优化概览

本次优化针对 bthread M:N 线程库的运行时性能，主要包括：

1. **热路径队列优化** - 自适应 spin，批量 pop，CAS weak
2. **同步原语优化** - 减少 atomic 操作，优化内存顺序
3. **Worker 调度优化** - 自适应 spin，轻量 RNG，无锁 wake
4. **内存布局优化** - TaskMeta 字段重排

## 性能基准

优化后基准测试结果（8 worker threads）：

| 基准测试 | 结果 |
|----------|------|
| Create/Join | 81,026 ops/sec |
| Yield | 8,002,961 yields/sec (125 ns/yield) |
| Mutex Contention | 11,028,945 lock/unlock/sec |
| vs std::thread | 3.19x faster |
| Scalability (8 workers) | 6.64x speedup vs 1 worker |
| Stack Performance | 148,750 ops/sec |
| Producer-Consumer | 492,732 items/sec |

**MPSC Queue 性能测试** (`tests/perf/mpsc_perf_test.cpp`):

| 测试 | 结果 |
|------|------|
| High contention (4 producers) | ~69K ops/sec |
| PopMultiple (batch 16) | ~7.2M ops/sec |
| Adaptive spin | ~1.1M ops/sec |

## 优化详情

### 1. MpscQueue Pop() 自适应 Spin

**文件**: `include/bthread/queue/mpsc_queue.hpp`

**问题**: Pop() 在 race condition 时立即 yield，导致不必要的 context switch。

**解决**:
- 使用 pause 指令在 x86 上进行低功耗 spin
- Spin 100 次后再 yield
- 其他架构使用 compiler barrier fallback

```cpp
// 优化前
while (!t->next.load(std::memory_order_acquire)) {
    std::this_thread::yield();
}

// 优化后
constexpr int MAX_SPINS = 100;
int spin_count = 0;
while (!t->next.load(std::memory_order_acquire)) {
    if (++spin_count < MAX_SPINS) {
        __builtin_ia32_pause();  // x86
    } else {
        std::this_thread::yield();
        spin_count = 0;
    }
}
```

### 2. Worker PickTask() 批量 Pop

**文件**: `src/bthread/core/worker.cpp`, `include/bthread/queue/work_stealing_queue.hpp`

**问题**: 从 local queue pop 任务时，多次调用 Pop() 产生不必要的 atomic 开销。

**解决**:
- 添加 `PopMultiple()` 方法到 WorkStealingQueue
- PickTask() 使用批量 pop 减少函数调用和 atomic 操作

### 3. WorkStealingQueue Steal() CAS 优化

**文件**: `src/bthread/queue/work_stealing_queue.cpp`

**问题**: Steal() 使用 `compare_exchange_strong`，在高竞争时开销较大。

**解决**:
- 使用 `compare_exchange_weak` + retry 循环
- 添加 `MAX_STEAL_ATTEMPTS = 3` 限制
- 失败时使用 relaxed 内存顺序

### 4. Butex Wait() Atomic 优化

**文件**: `src/bthread/sync/butex.cpp`

**问题**: Wait() 和 Wake() 中多处使用 acquire/release 内存顺序，部分可以安全地使用 relaxed。

**解决**:
- 初始 value_ 检查使用 relaxed（后续会再次检查）
- Wake() 中的 is_waiting 和 wake_count 使用 relaxed
- 关键的 state 操作保持 acquire/release

### 5. Mutex 快速路径优化

**文件**: `src/bthread/sync/mutex.cpp`

**问题**: lock_bthread() 直接使用 CAS，即使锁空闲也有 atomic 开销。

**解决**:
- 先用 relaxed load 检查锁状态
- 只在锁看起来空闲时才使用 CAS

### 6. Worker WaitForTask 自适应 Spin

**文件**: `src/bthread/core/worker.cpp`

**问题**: Worker 空闲时直接进入 futex wait，频繁任务提交时有内核调用开销。

**解决**:
- Spin 50 次后再进入 futex wait
- 每 5 次 spin 检查一次队列
- 使用 pause 指令减少功耗

### 7. Work Stealing 轻量 RNG

**文件**: `src/bthread/core/worker.cpp`

**问题**: 使用 `std::mt19937` 作为随机数生成器，开销较大。

**解决**:
- 使用 XOR shift 算法（3 次 XOR 操作）
- 比 mt19937 快得多，随机性足够用于窃取选择

### 8. Scheduler Wake 机制优化

**文件**: `include/bthread/core/scheduler.hpp`, `src/bthread/core/scheduler.cpp`

**问题**: `WakeIdleWorkers()` 需要获取 mutex，高频唤醒时有锁竞争。

**解决**:
- 添加 `workers_atomic_[]` 原子数组
- `WakeIdleWorkers()` 使用无锁访问
- `GetWorker()` 也使用无锁访问

### 9. TaskGroup Free List CAS 优化

**文件**: `src/bthread/core/task_group.cpp`

**问题**: `AllocTaskMeta()` 的 CAS 失败使用 acquire 内存顺序。

**解决**:
- 失败时使用 relaxed（只是 retry，不需要同步）
- 成功时仍使用 acq_rel

### 10. TaskMeta 内存布局优化

**文件**: `include/bthread/core/task_meta.hpp`

**问题**: TaskMeta 字段分散，热路径字段可能跨越多个 cache line。

**解决**:
- 按访问频率分组重排字段
- HOT: context switching, scheduling state
- WARM: synchronization
- COLD: entry/exit, join, other

## 注意事项

1. **Memory ordering**: 所有 relaxed 优化都经过仔细分析，确保不引入数据竞争
2. **平台兼容性**: pause 指令只在 x86 有效，其他平台有 fallback
3. **Spin 参数**: MAX_SPINS 等参数可根据具体工作负载调整

## 提交记录

- `b5f47f8` perf(queue): add adaptive spinning in MpscQueue Pop()
- `b8cc6a9` perf(worker): add PopMultiple and optimize PickTask batch
- `5c52db6` perf(queue): use CAS weak + retry in WorkStealingQueue Steal()
- `3f63687` perf(butex): reduce atomic operations with relaxed memory ordering
- `09bced8` perf(mutex): add relaxed load check in lock_bthread fast path
- `9578ba3` perf(worker): add adaptive spinning in WaitForTask
- `b661beb` perf(worker): use lightweight XOR shift RNG for work stealing
- `88838fa` perf(scheduler): use atomic array for lock-free worker wake
- `6758d02` perf(taskgroup): use relaxed memory ordering on CAS failure
- `afaf5ba` perf(taskmeta): optimize memory layout for cache locality