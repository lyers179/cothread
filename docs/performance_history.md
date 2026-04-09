# 性能优化历史记录

本文档记录了 bthread M:N 线程库的所有性能优化历史，按时间线整理。

---

## 2026-03-24 ~ 2026-03-25: M:N 线程池初始实现

**设计文档**: `docs/superpowers/specs/2026-03-24-m-n-thread-pool-design.md`
**实现计划**: `docs/superpowers/plans/2026-03-25-m-n-thread-pool.md`

### 核心功能
- Work-stealing 调度器
- bthread 创建、join、yield
- Mutex、Condition Variable 同步原语
- Butex 等待/唤醒机制

### 初始性能
- Create/Join: ~5,000 ops/sec
- bthread vs std::thread: **慢 6.92x**

---

## 2026-03-29: C++20 协程池支持

**设计文档**: `docs/superpowers/specs/2026-03-29-cpp20-coroutine-pool-design.md`
**实现计划**: `docs/superpowers/plans/2026-03-29-cpp20-coroutine-pool.md`

### 新增功能
- C++20 协程调度
- 协程与 bthread 统一调度
- 协程同步原语

---

## 2026-04-07: 第一轮性能优化

**设计文档**: `docs/superpowers/specs/2026-04-07-performance-optimization-design.md`
**实现计划**: `docs/superpowers/plans/2026-04-07-performance-optimization.md`

### 优化内容

| 优化项 | 说明 | 效果 |
|--------|------|------|
| MpscQueue 自适应 Spin | Pop() race 时先 spin 再 yield | 减少 context switch |
| WorkStealingQueue CAS weak | 使用 weak + retry 替代 strong | 减少原子操作开销 |
| Butex Wait 内存顺序 | 部分使用 relaxed | 减少内存屏障 |
| Mutex 快速路径 | 先 relaxed load 再 CAS | 减少无竞争开销 |
| Worker WaitForTask Spin | 空闲时先 spin 再 futex wait | 减少内核调用 |
| Work Stealing XOR RNG | 替换 mt19937 | 更快的随机数生成 |
| Scheduler 无锁 Wake | 原子数组替代 mutex | 消除锁竞争 |
| TaskMeta 内存布局 | 按访问频率重排字段 | 提高 cache locality |

### 性能结果（优化后）

| 基准测试 | 结果 |
|----------|------|
| Create/Join | 81,026 ops/sec |
| Yield | 8,002,961 yields/sec |
| Mutex Contention | 11,028,945 lock/unlock/sec |
| **vs std::thread** | **3.19x faster** |

---

## 2026-04-08: Bug 修复与重构

**设计文档**: `docs/superpowers/specs/2026-04-08-bthread-refactoring-design.md`
**实现计划**: `docs/superpowers/plans/2026-04-08-bthread-refactoring-plan.md`

### 修复内容
- Butex Wait/Wake 竞态条件修复
- 高竞争场景稳定性改进

---

## 2026-04-09: 分配开销优化（本次优化）

**设计文档**: `docs/superpowers/specs/2026-04-09-allocation-optimization-design.md`
**实现计划**: `docs/superpowers/plans/2026-04-09-allocation-optimization.md`

### 问题分析

bthread 创建时存在三个主要开销来源：

| 开销来源 | 原实现 | 影响 |
|----------|--------|------|
| Stack 分配 | 每次 mmap() (~8KB) | 系统调用开销高 |
| TaskMeta 分配 | 全局 CAS free list | 高竞争时冲突 |
| Butex 创建 | joinable 时立即创建 | 不必要时浪费 |

### 优化方案

#### Phase 1: Worker-local Stack Pool

每个 Worker 维护 8 个可复用栈：

```cpp
class Worker {
    static constexpr int STACK_POOL_SIZE = 8;
    void* stack_pool_[STACK_POOL_SIZE];
    int stack_pool_count_{0};
    
    void* AcquireStack(size_t size);
    void ReleaseStack(void* stack_top, size_t size);
};
```

- AcquireStack: 优先从池中获取，池空时再 mmap
- ReleaseStack: 返回到池中，池满时才 munmap

#### Phase 2: Worker-local TaskMeta Cache

每个 Worker 缓存 4 个 TaskMeta：

```cpp
class Worker {
    static constexpr int TASK_CACHE_SIZE = 4;
    TaskMeta* task_cache_[TASK_CACHE_SIZE];
    int task_cache_count_{0};
    
    TaskMeta* AcquireTaskMeta();
    void ReleaseTaskMeta(TaskMeta* meta);
};
```

- 批量从 TaskGroup 获取槽位（单次 CAS 获取多个）
- 本地缓存避免全局 CAS 竞争

#### Phase 3: Lazy Butex Allocation

Butex 只在首次 join 时创建：

```cpp
// bthread_create: 不创建 Butex
task->join_butex = nullptr;

// bthread_join: 懒加载
if (task->join_butex == nullptr) {
    Butex* new_butex = new Butex();
    void* expected = nullptr;
    if (!__atomic_compare_exchange_n(&task->join_butex, &expected, new_butex, ...)) {
        delete new_butex;  // 其他 joiner 已创建
    }
}
```

### 提交记录

| 提交 | 说明 |
|------|------|
| `91fb387` | feat(worker): add stack pool fields and method declarations |
| `21ee1f7` | test: add stack pool unit tests |
| `901aa80` | feat(worker): implement stack pool methods |
| `39eadd5` | feat(bthread): use worker stack pool in bthread_create |
| `a2d1909` | feat(worker): release stack to pool when bthread finishes |
| `372aa3e` | feat(worker): drain stack pool in destructor |
| `746f0b5` | feat(worker,task_group): add TaskMeta cache fields |
| `fbae941` | feat(taskmeta): implement worker-local TaskMeta cache |
| `e8574e3` | feat(bthread): implement lazy Butex allocation |
| `a5ed692` | fix(worker): keep stack with TaskMeta for reuse |

### 性能结果（修复后）

| 基准测试 | Phase 1 | Phase 2 (修复后) | 说明 |
|----------|---------|------------------|------|
| Create/Join | 81K ops/sec | **78-80K ops/sec** | 保持 |
| Yield | 8M/sec (125ns) | 8M/sec (124ns) | 保持 |
| Mutex Contention | 11M/sec | **12M/sec** | 略优 |
| **vs std::thread** | **快 3.19x** | **快 3.26x** | 保持 |
| Scalability (8w) | 7x | **6.5x** | 保持 |
| Stack Performance | 148K ops/sec | **152K ops/sec** | 略优 |
| Producer-Consumer | 492K items/sec | **519K items/sec** | 略优 |

### Bug 修复

**问题**: 在 HandleFinishedBthread 中释放 stack 到 worker 池导致性能回归。

**原因**:
- TaskMeta 被复用时没有 stack（已释放到池）
- bthread_create 需要重新分配 stack（mmap）
- 对于从主线程创建 bthread 的场景，worker 池从未被使用

**修复**: 保持 stack 与 TaskMeta 关联（类似 Phase 1），stack 池作为后备。

**关键成果**: bthread 保持比 std::thread 快 3.26x！（从慢 6.92x 改进约 22 倍）

---

## 性能演进总览

### 完整指标对比

| 指标 | 初始 (2026-03) | Phase 1 (2026-04-07) | Phase 2 (2026-04-09 修复后) | 改进幅度 |
|------|----------------|----------------------|----------------------------|----------|
| Create/Join | ~5,000 ops/sec | 81,026 ops/sec | **78,902 ops/sec** | **~16x** |
| Yield | - | 8M/sec (125ns) | 8M/sec (124ns) | 稳定 |
| Mutex Contention | - | 11M/sec | **12M/sec** | 稳定 |
| **vs std::thread** | **慢 6.92x** | **快 3.19x** | **快 3.26x** | **~22x** |
| Scalability (8w) | - | 7x | **6.5x** | 良好 |
| Stack Performance | - | 148K ops/sec | **152K ops/sec** | 稳定 |
| Producer-Consumer | - | 492K items/sec | **519K items/sec** | 稳定 |

### 关键改进

**bthread 从比 std::thread 慢 6.92x 变成快 3.26x！（约22倍改进）**

### 指标说明

| 指标 | 测试方法 | 说明 |
|------|----------|------|
| Create/Join | 10线程 × 10次迭代 | bthread 创建和销毁吞吐量 |
| Yield | 4线程 × 1000次 | 协程让步性能 |
| Mutex Contention | 4线程 × 1000次 | 高竞争锁性能 |
| vs std::thread | 4线程 × 10次 | 与原生线程对比 |
| Scalability | 1-16 workers | work-stealing 扩展性 |
| Stack Performance | 10线程 × 10次 | 栈分配压力测试 |
| Producer-Consumer | 2生产者 × 2消费者 | 消息传递吞吐量 |

```
2026-03-24          2026-04-07          2026-04-09 (修复后)
    │                   │                       │
    ▼                   ▼                       ▼
~5K ops/sec  ──────► 81K ops/sec  ──────►   79K ops/sec
    │                   │                       │
慢 6.92x           快 3.19x              快 3.26x
(相对std::thread)  (相对std::thread)     (相对std::thread)
```

---

## 未来优化方向

1. **大栈支持**: 当前池只支持 8KB 默认栈，大栈仍需 mmap
2. **跨 Worker 栈共享**: 可选地将溢出栈返回到全局池
3. **NUMA 感知**: 考虑 NUMA 拓扑的缓存分配
4. **动态池大小**: 根据工作负载动态调整池大小