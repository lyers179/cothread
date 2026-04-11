# Changelog

本文档记录 bthread M:N 线程库的所有重要变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/)。

---

## [Unreleased]

### Added
- 无

### Changed
- ButexQueue: MPSC → MPMC, PopFromHead 改用 CAS retry 实现多消费者安全
- Butex::Wake: 移除 wake_mutex_，实现无锁唤醒
- ExecutionQueue: 改用 MpscQueue，实现无锁任务提交

### Performance
- Concurrent Wake contention eliminated (多线程并发唤醒无锁竞争)
- ExecutionQueue Submit latency reduced (任务提交延迟降低)
- Mutex Contention latency: 0.09 µs/op

### Fixed
- 无

---

## [2026-04-11] - Futex Race Condition 修复

### Fixed

#### 静态析构顺序错误
- **问题**: `TaskGroup` 和 `Scheduler` 都是单例静态对象，C++ 静态对象析构顺序与构造顺序相反。`Scheduler` 析构函数调用 `GetTaskGroup()`，但此时 `TaskGroup` 可能已经析构。
- **表现**: 程序退出时 crash，抛出 `std::bad_alloc`
- **修复**: 在 `Scheduler` 构造函数中先调用 `GetTaskGroup()` 确保 TaskGroup 先构造、后析构
- **文件**: `src/bthread/core/scheduler.cpp:16-17`

#### Butex::Wake 动态分配异常
- **问题**: `Wake()` 函数使用 `std::vector::reserve()` 收集要唤醒的任务，在高并发场景下可能抛出 `std::bad_alloc`
- **修复**: 使用静态数组（16个元素）作为默认容器，仅在超过时才使用动态分配，并添加异常处理
- **文件**: `src/bthread/sync/butex.cpp:179-183`

#### Wait/Wake 双重入队竞态
- **问题**: 当 `Wake` 和 `Wait` 同时处理同一个任务时，双方都可能尝试将任务入队，导致任务被重复入队或丢失
- **修复**: 使用 CAS 操作确保只有一方成功入队
- **文件**: `src/bthread/sync/butex.cpp:145, 250`

#### 值变化检测遗漏
- **问题**: Wait 函数检查 butex 值后加入队列，但 Wake 可能在检查和入队之间改变值并唤醒，导致 Wait 进入无效等待
- **修复**: 加入队列后重新检查 butex 值，捕捉竞态
- **文件**: `src/bthread/sync/butex.cpp:112-120`

### Performance

修复后性能保持稳定：

| 基准测试 | 结果 |
|----------|------|
| Create/Join | 152K ops/sec (6.5 µs/op) |
| Yield | 32M yields/sec (31 ns/yield) |
| Mutex Contention | 19M lock/unlock/sec (0.05 µs/op) |
| **vs std::thread** | **10x faster** |
| Scalability (8 workers) | 7.8x speedup |
| Producer-Consumer | 750K items/sec |

**Benchmark 通过率**: 100%（从 30-70% 提升至稳定 100%）

---

## [2026-04-09] - 分配开销优化 + perf 分析优化

### Added

#### Worker-local Stack Pool
- 每个 Worker 维护 8 个可复用栈池
- AcquireStack 优先从池中获取，池空时再 mmap
- ReleaseStack 返回到池中，池满时才 munmap
- **文件**: `include/bthread/core/worker.hpp`, `src/bthread/core/worker.cpp`

#### Worker-local TaskMeta Cache
- 每个 Worker 缓存 4 个 TaskMeta
- 批量从 TaskGroup 获取槽位（单次 CAS 获取多个）
- 本地缓存避免全局 CAS 竞争
- **文件**: `src/bthread/core/task_group.cpp`

#### Lazy Butex Allocation
- Butex 只在首次 join 时创建
- 使用原子 CAS 防止重复创建
- **文件**: `src/bthread.cpp`

### Changed

#### Submit Wake 优化
- 用 `WakeIdleWorkers(1)` 替代 `WakeAllWorkers()`
- syscall 开销: 14.55% → 7.69%

#### WakeUp is_idle_ 标志
- 添加 `is_idle_` 标志避免不必要的 futex 调用
- 只在 worker 实际等待时才调用 futex wake

### Performance

| 指标 | 优化前 | 优化后 | 改进 |
|------|--------|--------|------|
| Scalability (4w) | 6.56x | 8.67x | +32% |
| Scalability (8w) | 6.56x | 8.50x | +30% |
| Producer-Consumer | 519K | 728K | +40% |
| Stack Performance | 152K | 162K | +7% |

---

## [2026-04-07] - 第一轮性能优化

### Changed

#### MpscQueue Pop() 自适应 Spin
- 使用 pause 指令在 x86 上进行低功耗 spin
- Spin 100 次后再 yield
- **文件**: `include/bthread/queue/mpsc_queue.hpp`

#### WorkStealingQueue CAS weak
- 使用 `compare_exchange_weak` + retry 替代 `compare_exchange_strong`
- 添加 `MAX_STEAL_ATTEMPTS = 3` 限制
- **文件**: `src/bthread/queue/work_stealing_queue.cpp`

#### Butex Wait 内存顺序优化
- 初始 value_ 检查使用 relaxed（后续会再次检查）
- Wake() 中的 is_waiting 和 wake_count 使用 relaxed
- **文件**: `src/bthread/sync/butex.cpp`

#### Mutex 快速路径优化
- 先用 relaxed load 检查锁状态
- 只在锁看起来空闲时才使用 CAS
- **文件**: `src/bthread/sync/mutex.cpp`

#### Worker WaitForTask 自适应 Spin
- Spin 50 次后再进入 futex wait
- 每 5 次 spin 检查一次队列
- **文件**: `src/bthread/core/worker.cpp`

#### Work Stealing 轻量 RNG
- 使用 XOR shift 算法（3 次 XOR 操作）
- 替换开销较大的 `std::mt19937`
- **文件**: `src/bthread/core/worker.cpp`

#### Scheduler 无锁 Wake
- 添加 `workers_atomic_[]` 原子数组
- `WakeIdleWorkers()` 和 `GetWorker()` 使用无锁访问
- **文件**: `include/bthread/core/scheduler.hpp`, `src/bthread/core/scheduler.cpp`

#### TaskMeta 内存布局优化
- 按访问频率分组重排字段
- HOT → WARM → COLD
- **文件**: `include/bthread/core/task_meta.hpp`

### Performance

| 基准测试 | 优化前 | 优化后 |
|----------|--------|--------|
| Create/Join | ~5K ops/sec | 81K ops/sec |
| Yield | - | 8M yields/sec |
| Mutex Contention | - | 11M lock/unlock/sec |
| **vs std::thread** | **慢 6.92x** | **快 3.19x** |

---

## [2026-03-29] - C++20 协程池支持

### Added
- C++20 协程调度框架
- 协程与 bthread 统一调度器
- 协程同步原语（CoMutex, CoCondVar）
- 协程 sleep 支持
- Task/Result 类型系统

---

## [2026-03-24] - M:N 线程池初始实现

### Added
- Work-stealing 调度器
- bthread 创建、join、yield API
- Mutex、Condition Variable 同步原语
- Butex 等待/唤醒机制
- Global Queue 和 Local Queue
- 多平台支持（Linux, Windows）

### Initial Performance
- Create/Join: ~5,000 ops/sec
- bthread vs std::thread: **慢 6.92x**