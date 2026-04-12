# Bthread 性能优化说明

## 优化概览

本次优化针对 bthread M:N 线程库的运行时性能，分为三个阶段：

### 第一阶段 (2026-04-07)
1. **热路径队列优化** - 自适应 spin，批量 pop，CAS weak
2. **同步原语优化** - 减少 atomic 操作，优化内存顺序
3. **Worker 调度优化** - 自适应 spin，轻量 RNG，无锁 wake
4. **内存布局优化** - TaskMeta 字段重排

### 第二阶段 (2026-04-09) - 分配开销优化
5. **Worker-local Stack Pool** - 复用栈避免 mmap/munmap
6. **Worker-local TaskMeta Cache** - 批量分配减少 CAS 竞争
7. **Lazy Butex Allocation** - 按需创建 join 同步对象

### 第三阶段 (2026-04-11) - Futex 竞态修复
8. **静态析构顺序修复** - 确保正确析构顺序
9. **Butex::Wake 分配安全** - 静态数组避免异常
10. **Wait/Wake 双重入队防护** - CAS 确保单次入队
11. **Butex 值重检** - 入队后重新检查值变化

### 第四阶段 (2026-04-11) - Lock-Free Queue 优化
12. **ButexQueue MPMC PopFromHead** - CAS retry 实现多消费者安全
13. **Butex Wake 无锁** - 移除 wake_mutex_，消除并发唤醒竞争
14. **ExecutionQueue 无锁提交** - 改用 MpscQueue，无锁任务提交

### 第五阶段 (2026-04-12) - Pause/Yield 优化
15. **自适应 Spin** - pause 指令优先，yield 作为 fallback
16. **批量 Pop** - PopMultipleFromHead 减少 CAS 开销
17. **内存序优化** - seq_cst → acquire 减少同步开销

### 第六阶段 (2026-04-12) - 全面性能优化
18. **WakeIdleWorkers 选择性唤醒** - IdleRegistry 注册空闲 worker
19. **Mutex Lock-Free Waiter** - MpscQueue 替代 waiter_list_ mutex
20. **Global Queue MPMC** - ShardedGlobalQueue 分片队列
21. **Timer Sharding** - Per-shard mutex 降低 contention
22. **Yield Fast Path** - 无竞争时跳过队列操作

## 性能基准

### 第六阶段优化后（2026-04-12）

| 基准测试 | 结果 |
|----------|------|
| Create/Join | ~110K ops/sec (9 µs/op) |
| Yield | **~79M yields/sec (13 ns/yield)** |
| Mutex Contention | **~23M lock/unlock/sec (0.04 µs/op)** |
| **vs std::thread** | **~11.5x faster** |
| Scalability (8 workers) | **~12x speedup** |
| Stack Performance | **~341K ops/sec** |
| Producer-Consumer | **~731K items/sec** |
| **Benchmark 通过率** | **100%** |

### 完整指标对比

| 指标 | 初始 | Phase 1 | Phase 2 | Phase 3 | Phase 4 | Phase 5 | Phase 6 (最新) |
|------|------|---------|---------|---------|---------|---------|----------------|
| Create/Join | ~5K | 81K | 83K | 152K | 92K | ~110K | **~110K** |
| vs std::thread | 慢 6.92x | 快 3.19x | 快 3.15x | 快 10x | 快 3.79x | 快 3.5x | **快 ~11.5x** |
| Scalability (8w) | - | 6.64x | 8.50x | 7.8x | 5.86x | 5.6x | **~12x** |
| Stack Performance | - | 148K | 162K | 298K | 142K | 140K | **~341K** |
| Producer-Consumer | - | 492K | 728K | 750K | 463K | 460K | **~731K** |
| Yield | - | 8M/sec | 8M/sec | 32M/sec | 8M/sec | 8M/sec | **~79M/sec** |
| Mutex Contention | - | 11M/sec | 12M/sec | 19M/sec | 12M/sec | 12M/sec | **~23M/sec** |

### perf 分析（第六阶段）

perf 分析结果显示：
- 33% syscall (futex 等)
- 17% Mutex::unlock
- 17% TaskGroup::GetSuspendedTasks
- 17% bthread_join

**关键发现：Yield 性能从 8M/sec 暴增至 ~79M/sec (+888%)**
原因：Yield Fast Path 在无竞争时跳过队列操作，延迟从 ~125ns 降至 ~13ns

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

### 第一阶段 (2026-04-07)
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

### 第二阶段 (2026-04-09) - 分配开销优化
- `91fb387` feat(worker): add stack pool fields and method declarations
- `21ee1f7` test: add stack pool unit tests
- `901aa80` feat(worker): implement stack pool methods
- `39eadd5` feat(bthread): use worker stack pool in bthread_create
- `a2d1909` feat(worker): release stack to pool when bthread finishes
- `372aa3e` feat(worker): drain stack pool in destructor
- `746f0b5` feat(worker,task_group): add TaskMeta cache fields
- `fbae941` feat(taskmeta): implement worker-local TaskMeta cache
- `e8574e3` feat(bthread): implement lazy Butex allocation

### 第三阶段 (2026-04-11) - Futex 竞态修复
- `scheduler.cpp:16-17` fix: ensure TaskGroup constructed before Scheduler
- `butex.cpp:179-183` fix: use static array in Wake to avoid allocation exception
- `butex.cpp:145,250` fix: use CAS to prevent double enqueue
- `butex.cpp:112-120` fix: re-check butex value after adding to queue

### 第四阶段 (2026-04-11) - Lock-Free Queue 优化
- `butex_queue.cpp` feat: MPMC PopFromHead with CAS retry and timeout safety
- `butex.hpp/cpp` feat: remove wake_mutex_ for lock-free Wake
- `execution_queue.hpp/cpp` feat: use MpscQueue for lock-free submit
- `mpsc_queue.hpp` fix: clear next pointer in Pop() for correct behavior

---

## 第二阶段优化详情 (2026-04-09)

### 11. Worker-local Stack Pool

**文件**: `include/bthread/core/worker.hpp`, `src/bthread/core/worker.cpp`

**问题**: 每次 bthread 创建都调用 mmap() 分配栈，系统调用开销高。

**解决**:
- 每个 Worker 维护 8 个可复用栈池
- AcquireStack 优先从池中获取
- ReleaseStack 返回到池中
- 池满或非默认大小时才 munmap

```cpp
class Worker {
    static constexpr int STACK_POOL_SIZE = 8;
    static constexpr size_t DEFAULT_STACK_SIZE = 8192;
    
    void* stack_pool_[STACK_POOL_SIZE];
    int stack_pool_count_{0};
    
    void* AcquireStack(size_t size);
    void ReleaseStack(void* stack_top, size_t size);
    void DrainStackPool();
};
```

### 12. Worker-local TaskMeta Cache

**文件**: `include/bthread/core/worker.hpp`, `src/bthread/core/worker.cpp`, `src/bthread/core/task_group.cpp`

**问题**: TaskMeta 分配使用全局 CAS free list，高竞争时冲突严重。

**解决**:
- 每个 Worker 缓存 4 个 TaskMeta
- 批量从 TaskGroup 获取槽位（单次 CAS 获取多个）
- 本地缓存避免全局竞争

```cpp
class Worker {
    static constexpr int TASK_CACHE_SIZE = 4;
    TaskMeta* task_cache_[TASK_CACHE_SIZE];
    int task_cache_count_{0};
    
    TaskMeta* AcquireTaskMeta();
    void ReleaseTaskMeta(TaskMeta* meta);
    void RefillTaskCache();
    void DrainTaskCache();
};

// TaskGroup 批量分配
int TaskGroup::AllocMultipleSlots(int32_t* slots, int count);
TaskMeta* TaskGroup::GetOrCreateTaskMeta(int32_t slot);
```

### 13. Lazy Butex Allocation

**文件**: `src/bthread.cpp`

**问题**: joinable bthread 创建时立即分配 Butex，即使没有 join 操作也浪费。

**解决**:
- bthread_create 不创建 Butex
- bthread_join 时懒加载，使用原子 CAS 防止重复创建

```cpp
// bthread_create
task->join_butex = nullptr;  // 不预分配

// bthread_join
if (task->join_butex == nullptr) {
    Butex* new_butex = new Butex();
    void* expected = nullptr;
    if (!__atomic_compare_exchange_n(&task->join_butex, &expected, new_butex,
            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        delete new_butex;  // 其他 joiner 已创建
    }
}
```

---

## 第三阶段优化详情 (2026-04-11)

### 14. 静态析构顺序修复

**文件**: `src/bthread/core/scheduler.cpp`

**问题**: `TaskGroup` 和 `Scheduler` 都是单例静态对象，C++ 静态析构顺序与构造顺序相反。`Scheduler` 析构函数调用 `GetTaskGroup()`，但此时 `TaskGroup` 可能已析构。

**表现**: 程序退出时 crash，抛出 `std::bad_alloc`

**解决**:
```cpp
Scheduler::Scheduler() {
    // 确保 TaskGroup 先构造，后析构
    (void)GetTaskGroup();
}
```

**原理**: 构造顺序：TaskGroup → Scheduler；析构顺序：Scheduler → TaskGroup

### 15. Butex::Wake 分配安全

**文件**: `src/bthread/sync/butex.cpp`

**问题**: `Wake()` 使用 `std::vector::reserve()` 收集要唤醒的任务，在高并发下可能抛出 `std::bad_alloc`

**解决**:
```cpp
// 使用静态数组避免动态分配
constexpr int STATIC_SIZE = 16;
TaskMeta* static_tasks[STATIC_SIZE];
std::vector<TaskMeta*> dynamic_tasks;
TaskMeta** tasks_to_wake = static_tasks;
int tasks_capacity = STATIC_SIZE;

// 仅在超过静态容量时才使用动态分配，并添加异常处理
try {
    if (dynamic_tasks.empty()) {
        dynamic_tasks.reserve(std::max(count, 64));
        ...
    }
} catch (...) {
    // 分配失败时立即唤醒该任务
    waiter->state.store(TaskState::READY, std::memory_order_release);
    Scheduler::Instance().EnqueueTask(waiter);
}
```

**原理**: 大多数唤醒操作只唤醒 1-16 个任务，静态数组足够且不抛异常

### 16. Wait/Wake 双重入队防护

**文件**: `src/bthread/sync/butex.cpp`

**问题**: 当 `Wake` 和 `Wait` 同时处理同一任务时，双方都可能入队

**解决**:
```cpp
// Wait 中：检测到 Wake 后使用 CAS
TaskState expected = TaskState::SUSPENDED;
if (task->state.compare_exchange_strong(expected, TaskState::READY, ...)) {
    Scheduler::Instance().EnqueueTask(task);  // CAS 成功才入队
}

// Wake 中：唤醒时使用 CAS
TaskState expected = TaskState::SUSPENDED;
if (waiter->state.compare_exchange_strong(expected, TaskState::READY, ...)) {
    Scheduler::Instance().EnqueueTask(waiter);  // CAS 成功才入队
}
```

**原理**: CAS 确保只有一方成功入队，失败方知道对方已处理

### 17. Butex 值重检

**文件**: `src/bthread/sync/butex.cpp`

**问题**: Wait 检查 butex 值后入队，Wake 可能在检查和入队之间改变值

**解决**:
```cpp
// 7.5. 入队后重新检查值
val = value_.load(std::memory_order_acquire);
if (val != expected_value) {
    // 值已变，标记节点为已认领，退出等待
    node->claimed.store(true, std::memory_order_release);
    task->is_waiting.store(false, std::memory_order_release);
    task->waiting_butex = nullptr;
    return 0;
}
```

**原理**: 捕捉 Wake 在入队前已改变值的竞态

### 内存序要点

第三阶段修复中使用了正确的内存序：

| 操作 | 内存序 | 原因 |
|------|--------|------|
| wake_count/wake_pending | `seq_cst` | 关键同步点，需要全局可见顺序 |
| CAS 操作 | `acq_rel` | 修改并需要看到对方状态 |
| 读取对方修改的状态 | `acquire` | 同步对方 release 的修改 |
| 修改状态让对方看到 | `release` | 让对方的 acquire 可见 |

---

## 性能历史文档

完整性能优化历史请参考: `docs/performance_history.md`
