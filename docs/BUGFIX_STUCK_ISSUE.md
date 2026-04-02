# M:N 线程库卡死问题修复文档

## 问题概述

bthread 库在 benchmark 和 stress_test 中出现偶发性卡死问题，主要表现为：
- 高并发 mutex 操作时无限等待
- 条件变量 signal/wait 后无法正确唤醒
- bthread_join 在任务完成后仍阻塞
- 快速创建/销毁 bthread 时偶发死锁

---

## 调试历程

### 阶段一：问题复现与初步定位

**现象：**
- 运行 `benchmark.exe` 时，Test 3 (Mutex Contention) 在 8线程×10000次迭代时几乎 100% 卡死
- 降低到 8线程×200次迭代时可以正常通过

**调试思路：**
1. **二分法定位**：逐步降低迭代次数，找到卡死的临界点
2. **添加日志**：在关键路径添加计数器和日志，观察哪个线程卡住

**初步分析：**
- 卡死发生在高竞争场景，怀疑是公平性问题
- M:N 线程模型中，释放锁的线程立即重新竞争，比刚被唤醒的线程有优势

### 阶段二：尝试的失败方案

#### 方案 A：解锁后让出 CPU

```cpp
int bthread_mutex_unlock(bthread_mutex_t* mutex) {
    // ... 解锁逻辑 ...
    butex->Wake(1);
    bthread_yield();  // 让出 CPU，给被唤醒线程机会
}
```

**结果：** 失败，卡死问题依然存在

**原因分析：** `bthread_yield()` 只是让当前 bthread 让出，但调度器可能立即重新调度它，没有解决根本问题

#### 方案 B：实现 handoff 机制

```cpp
// 在 mutex 中添加 handoff 字段
struct bthread_mutex_t {
    std::atomic<void*> butex;
    std::atomic<uint64_t> owner;
    std::atomic<TaskMeta*> handoff;  // 直接传递给等待者
};
```

**结果：** 出现 segfault

**原因分析：**
- handoff 标志的竞态条件：解锁线程设置 handoff 时，等待者可能已经被唤醒
- 多个等待者竞争 handoff，导致状态混乱

#### 方案 C：唤醒任务放入全局队列

```cpp
void Butex::Wake(int count) {
    // ...
    Scheduler::Instance().EnqueueTaskGlobal(waiter);  // 放入全局队列而非本地队列
}
```

**结果：** 失败

**原因分析：** 问题不在队列位置，而在等待队列的顺序

### 阶段三：研究官方实现

**关键发现：** 查看 `benchmark/bthread/mutex.cpp` 发现官方使用了特殊策略：

```cpp
// 官方 bthread mutex.cpp:1064-1086
bool queue_lifo = false;
bool first_wait = true;
while (whole->exchange(BTHREAD_MUTEX_CONTENDED) & BTHREAD_MUTEX_LOCKED) {
    if (bthread::butex_wait(whole, BTHREAD_MUTEX_CONTENDED, abstime, queue_lifo) < 0) {
        // ...
    }
    if (!first_wait && 0 == errno) {
        // 关键！唤醒后竞争失败，使用 LIFO 给刚唤醒的线程更好的机会
        queue_lifo = true;
    }
    first_wait = false;
}
```

**核心洞察：**
1. **第一次等待**：使用 FIFO（公平），新来的线程排队
2. **唤醒后失败**：使用 LIFO（优先），刚唤醒的线程放在队列头部，下次更容易获得锁

这解决了公平性问题的本质：
- 如果只用 FIFO：新来的线程和刚唤醒的线程公平竞争，但新来的线程已经在 CPU 上运行，有优势
- 如果只用 LIFO：最近等待的线程总是先被唤醒，可能导致其他线程饥饿
- **混合策略**：第一次 FIFO 保证公平，后续 LIFO 补偿唤醒线程的劣势

### 阶段四：实现解决方案

**第一步：修改 Butex 支持两种插入方式**

从 Treiber 栈（无锁 LIFO）改为双向链表：
- 原来的 Treiber 栈只支持头部插入（LIFO）
- 需要支持尾部插入（FIFO）

**第二步：添加 in_queue 标志**

发现竞态条件：`RemoveFromWaitQueue` 可能在任务已被 `PopFromHead` 移除后被调用。

解决方案：添加 `in_queue` 原子标志，`RemoveFromWaitQueue` 检查标志后再操作。

**第三步：修改 Mutex 使用混合策略**

```cpp
bool first_wait = true;
while (true) {
    // ... 尝试获取锁 ...
    butex->Wait(generation, nullptr, !first_wait);  // 第一次FIFO，后续LIFO
    first_wait = false;
}
```

### 阶段五：验证结果

**测试结果：**
```
=== Run 1 === [Benchmark 3] Expected: 80000, Actual: 80000, Throughput: 7838066
=== Run 2 === [Benchmark 3] Expected: 80000, Actual: 80000, Throughput: 7311278
=== Run 3 === [Benchmark 3] Expected: 80000, Actual: 80000, Throughput: 7330774
=== Run 4 === [Benchmark 3] Expected: 80000, Actual: 80000, Throughput: 9683003
=== Run 5 === [Benchmark 3] Expected: 80000, Actual: 80000, Throughput: 10994599
```

**从 100% 卡死到 10 次测试全部通过！**

---

## 调试经验总结

### 1. 问题定位方法

| 方法 | 适用场景 | 本次应用 |
|------|----------|----------|
| **二分法** | 确定问题的临界条件 | 降低迭代次数找到卡死的阈值 |
| **对比法** | 有参考实现时 | 对比官方 bthread 实现 |
| **日志法** | 需要了解运行时状态 | 在关键路径添加计数器 |
| **假设验证** | 有多个可能原因 | 依次尝试不同解决方案 |

### 2. 常见陷阱

| 陷阱 | 表现 | 本案例 |
|------|------|--------|
| **症状修复** | 修复了表面问题，根因仍存在 | yield() 没有解决公平性问题 |
| **过度复杂** | 引入新机制导致新 bug | handoff 方案引入新的竞态条件 |
| **忽视参考** | 自己造轮子，忽略成熟方案 | 官方已有成熟的解决方案 |
| **过早优化** | 在正确之前追求高效 | 先保证正确，再考虑性能 |

### 3. M:N 线程模型的关键问题

```
┌─────────────────────────────────────────────────────────────┐
│                  M:N 线程模型的公平性挑战                     │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   Thread A (持有锁)           Thread B (等待锁)              │
│        │                           │                        │
│   unlock()                    wait in queue                 │
│        │                           │                        │
│   立即循环尝试                被唤醒，但还未被调度             │
│   重新竞争                    到 CPU 运行                    │
│        │                           │                        │
│   ▼ 已在 CPU 运行            ▼ 需要等待调度                  │
│        │                           │                        │
│   竞争优势大                  竞争劣势大                      │
│                                                             │
│   解决方案：FIFO/LIFO 混合策略                               │
│   - 第一次 FIFO：新线程公平排队                              │
│   - 后续 LIFO：唤醒线程优先重试                              │
└─────────────────────────────────────────────────────────────┘
```

### 4. 学习官方实现的价值

官方 bthread 代码中的注释和设计决策：
- `mutex.cpp:1079-1085` 注释明确解释了为什么要用 LIFO
- 变量名 `queue_lifo` 清晰表达了意图
- 代码结构展示了渐进式策略的应用

**教训：遇到问题时，首先查看成熟的开源实现如何解决类似问题**

### 5. 调试高并发问题的技巧

#### 5.1 确定问题类型

```
问题卡死 → 是死锁还是活锁？
    │
    ├─ 死锁：线程永久等待
    │   └─ 检查锁的获取顺序、资源依赖
    │
    └─ 活锁：线程在运行但无进展
        └─ 检查公平性、优先级反转
```

**本案例是活锁**：线程都在运行，但由于公平性问题，某些线程永远无法获得锁。

#### 5.2 竞态条件调试

**方法：添加诊断计数器**

```cpp
static std::atomic<int> wait_count{0};
static std::atomic<int> wake_count{0};
static std::atomic<int> wakeup_enqueues{0};

// 在 Wait 中
wait_count++;

// 在 Wake 中
wake_count++;
if (state == TaskState::SUSPENDED) {
    wakeup_enqueues++;
}

// 定期打印诊断信息
fprintf(stderr, "wait=%d wake=%d enqueue=%d\n",
        wait_count.load(), wake_count.load(), wakeup_enqueues.load());
```

**发现：** `wake_count` 和 `wakeup_enqueues` 不匹配，说明唤醒后任务没有被正确调度。

#### 5.3 理解等待队列顺序的影响

```
LIFO 栈结构（Treiber栈）：
    ┌───┐
    │ C │ ← 新等待者加入头部
    ├───┤
    │ B │
    ├───┤
    │ A │ ← 最早等待者
    └───┘
    Wake 时先唤醒 C

问题：如果 A 刚被唤醒但还没运行，C 新加入后被唤醒
      A 醒来后发现锁又被抢走了

FIFO 队列结构：
    ┌───┐
    │ A │ ← 最早等待者
    ├───┤
    │ B │
    ├───┤
    │ C │ ← 新等待者加入尾部
    └───┘
    Wake 时先唤醒 A

问题：新来的线程 D 可能比刚唤醒的 A 更快获得锁
      因为 D 已经在 CPU 上运行
```

**混合策略的精髓：**
- 第一次等待用 FIFO：新线程 D 排在后面，公平
- 唤醒后失败用 LIFO：A 放在队头，下次优先获得锁，补偿其调度延迟

### 6. 代码审查清单

调试并发问题时的检查清单：

- [ ] **原子操作顺序**：是否存在 TOCTOU（Time Of Check To Time Of Use）问题？
- [ ] **内存序**：是否使用了正确的 memory_order？
- [ ] **状态转换**：状态机是否完整，是否有遗漏的转换？
- [ ] **唤醒丢失**：Wake 在 Wait 之前发生怎么办？
- [ ] **虚假唤醒**：是否在循环中检查条件？
- [ ] **公平性**：是否存在饥饿问题？
- [ ] **资源泄漏**：异常路径是否正确释放资源？

### 7. 本案例的关键代码审查发现

| 发现 | 文件 | 行号 | 问题 |
|------|------|------|------|
| value 永久设为 1 | butex.cpp | 18 | Wake 后 value 不重置 |
| join 竞态 | bthread.cpp | 102-118 | 先检查状态再获取 generation |
| Wake/Wait 竞态 | butex.cpp | 60-72 | 状态检查顺序错误 |
| LIFO 公平性 | mutex.cpp | 全局 | 等待队列顺序导致饥饿 |

---

## 根因分析

### 问题 1: Butex value 永久设置为 1

**原始代码问题：**
```cpp
void Butex::Wake(int count) {
    value_.store(1, std::memory_order_release);  // 问题：值永远设为 1
    // ...
}

int Butex::Wait(int expected_value, ...) {
    if (value_.load() != expected_value) {  // 问题：如果 expected=0，第一次 Wake 后永远返回
        return 0;
    }
    // ...
}
```

**问题分析：**
- `Wake()` 将 `value_` 设为 1 后永远不会重置
- 后续所有 `Wait(0)` 都会立即返回，导致无限循环
- 影响条件变量和 mutex 的正确性

### 问题 2: bthread_join 竞态条件

**原始代码问题：**
```cpp
int bthread_join(bthread_t tid, void** retval) {
    // 检查状态
    if (task->state.load() == TaskState::FINISHED) {
        return 0;
    }
    // 问题：在检查状态和获取 generation 之间，任务可能完成
    static_cast<Butex*>(task->join_butex)->Wait(0, nullptr);
    // ...
}
```

**问题分析：**
- 任务可能在状态检查和 Wait 调用之间完成
- 此时 Wake 已执行，但等待者还没开始等待
- 导致 join 永远阻塞

### 问题 3: Wake/Wait 竞态条件

**原始代码问题：**
```cpp
void Butex::Wake(int count) {
    // 问题：从队列取出等待者后，等待者可能还没挂起
    if (ws.wakeup.compare_exchange_strong(expected, true, ...)) {
        waiter->state.store(TaskState::READY, ...);
        Scheduler::Instance().EnqueueTask(waiter);  // 问题：任务可能被执行两次
    }
}

int Butex::Wait(...) {
    waiters_.compare_exchange_weak(...);  // 加入队列
    // 问题：Wake 可能在此时取出等待者
    task->state.store(TaskState::SUSPENDED, ...);
    w->SuspendCurrent();  // 问题：任务已在运行队列中，但仍挂起
}
```

**问题分析：**
- Wake 从队列取出等待者时，等待者可能还没执行 SuspendCurrent
- Wake 将任务放入运行队列，但任务后续仍会挂起
- 导致任务状态混乱

### 问题 4: Worker 等待唤醒遗漏

**原始代码问题：**
```cpp
void Worker::WaitForTask() {
    sleeping_.store(true, ...);
    if (!local_queue_.Empty() || !global_queue.Empty()) {
        return;
    }
    // 问题：检查队列和读取 sleep_token 之间，新任务可能入队
    int expected = sleep_token_.load(...);
    platform::FutexWait(&sleep_token_, expected, nullptr);  // 可能永远阻塞
}
```

**问题分析：**
- 任务入队和 WakeUp 之间存在时序窗口
- Worker 可能错过唤醒信号

### 问题 5: 内存泄漏

**原始代码问题：**
```cpp
void TaskGroup::DeallocTaskMeta(TaskMeta* task) {
    // 问题：join_butex 从未被删除
    task->join_butex = nullptr;  // 只置空，不删除
    // ...
}
```

**问题分析：**
- 每次 bthread_create 都会 new Butex()
- DeallocTaskMeta 不删除，导致内存泄漏
- 高频创建/销毁会耗尽内存

## 解决方案

### 方案 1: 使用 Generation 机制

**核心思想：** 使用递增的 generation 代替固定值 0/1

```cpp
// Wait 等待 generation 变化
int generation = butex->value();
butex->Wait(generation, nullptr);

// Wake 增加 generation 并唤醒
butex->set_value(butex->value() + 1);
butex->Wake(1);
```

**应用到：**
- 条件变量 `bthread_cond_signal/bthread_cond_wait`
- Mutex `bthread_mutex_unlock/bthread_mutex_lock`
- bthread_join 的完成通知

### 方案 2: 修复 Wake/Wait 竞态

```cpp
int Butex::Wait(int expected_value, ...) {
    // 1. 加入等待队列
    waiters_.compare_exchange_weak(...);

    // 2. 设置 SUSPENDED 状态
    task->state.store(TaskState::SUSPENDED, ...);

    // 3. 检查 wakeup 标志（在 SUSPENDED 之后）
    if (ws.wakeup.load(...)) {
        // Wake 已经执行，恢复状态并返回
        task->state.store(TaskState::READY, ...);
        return 0;
    }

    // 4. 安全挂起
    w->SuspendCurrent();
}

void Butex::Wake(int count) {
    if (ws.wakeup.compare_exchange_strong(expected, true, ...)) {
        // 检查任务是否已经 SUSPENDED
        if (waiter->state.load(...) == TaskState::SUSPENDED) {
            // 只有 SUSPENDED 状态才放入运行队列
            waiter->state.store(TaskState::READY, ...);
            Scheduler::Instance().EnqueueTask(waiter);
        }
        // 如果不是 SUSPENDED，任务会检查 wakeup 并正确返回
    }
}
```

### 方案 3: 修复 bthread_join 竞态

```cpp
int bthread_join(bthread_t tid, void** retval) {
    // 先获取 generation，再检查状态
    Butex* join_butex = static_cast<Butex*>(task->join_butex);
    int generation = join_butex->value();

    // 检查状态
    if (task->state.load() == TaskState::FINISHED) {
        return 0;
    }

    // 等待 generation 变化
    join_butex->Wait(generation, nullptr);
}
```

### 方案 4: 修复 Worker 等待

```cpp
void Worker::WaitForTask() {
    sleeping_.store(true, ...);

    // 第一次检查
    if (!local_queue_.Empty() || !global_queue.Empty()) {
        sleeping_.store(false, ...);
        return;
    }

    int expected = sleep_token_.load(...);

    // 第二次检查（在读取 token 之后）
    if (!local_queue_.Empty() || !global_queue.Empty()) {
        sleeping_.store(false, ...);
        return;
    }

    platform::FutexWait(&sleep_token_, expected, nullptr);
    sleeping_.store(false, ...);
}
```

### 方案 5: 修复内存泄漏

```cpp
void TaskGroup::DeallocTaskMeta(TaskMeta* task) {
    // 删除 join_butex
    if (task->join_butex) {
        delete static_cast<bthread::Butex*>(task->join_butex);
        task->join_butex = nullptr;
    }
    // ... 其他清理
}
```

## 修改的文件

| 文件 | 修改内容 |
|------|----------|
| `src/butex.cpp` | 修复 Wake/Wait 竞态条件，添加正确的状态检查 |
| `src/cond.cpp` | 使用 generation 机制实现 signal/wait |
| `src/mutex.cpp` | 使用 generation 机制，添加 double-check |
| `src/bthread.cpp` | 修复 bthread_join 竞态，正确获取 generation |
| `src/worker.cpp` | 修复 WaitForTask 竞态，添加 HandleFinishedTask 的 generation 增加 |
| `src/task_group.cpp` | 修复 join_butex 内存泄漏 |
| `include/bthread/platform/platform.h` | 修复 MSVC 下 EINVAL 定义问题 |
| `benchmark/CMakeLists.txt` | 添加 EINVAL 定义 |

## 测试结果

### 修复前

| 测试 | 状态 |
|------|------|
| demo | ❌ 偶发卡死 |
| bthread_test | ❌ 偶发卡死 |
| stress_test | ❌ 频繁卡死 |
| benchmark | ❌ 频繁卡死 |

### 第一轮修复后

| 测试 | 状态 | 说明 |
|------|------|------|
| demo | ✅ 100% 通过 | 所有功能正常 |
| bthread_test | ✅ 100% 通过 | API 测试稳定 |
| stress_test Test 1 | ✅ ~90%+ 通过 | 高并发 (100线程×100次) |
| stress_test Test 2 | ✅ 100% 通过 | 深度递归 (50层) |
| stress_test Test 3 | ⚠️ ~40% 通过 | 快速创建/销毁 (1000次) |
| benchmark Test 1 | ✅ 100% 通过 | Create/Join: ~600K ops/sec |
| benchmark Test 2 | ✅ 100% 通过 | Yield: ~60-90M yields/sec |
| benchmark Test 3 | ❌ 卡住 | 高并发 mutex (8线程×10000次) |

### 第二轮修复后 (FIFO/LIFO混合策略)

| 测试 | 状态 | 说明 |
|------|------|------|
| benchmark Test 3 | ✅ 100% 通过 | 高并发 mutex (8线程×10000次，共80000次操作) |
| 吞吐量 | ~7-11M ops/sec | 稳定，无卡死 |

## 已知限制

### 1. ~~极端高并发 Mutex 场景~~ (已修复)

**原问题描述：**
- 8 个线程各执行 10000 次 mutex lock/unlock（共 80000 次操作）会卡住
- 较低并发（如 200 次迭代）通常正常工作

**根本原因（已确认）：**
- Butex 等待队列使用 LIFO 栈结构（Treiber栈）
- 释放线程立即重新竞争，而刚被唤醒的线程还未被调度
- 唤醒后的线程在队列尾部，总是最后获得锁

**最终解决方案：**
参考官方 bthread 实现，采用 FIFO/LIFO 混合策略：
1. **第一次等待**：使用 FIFO（append to tail），保证公平性
2. **唤醒后竞争失败**：使用 LIFO（prepend to head），给刚唤醒的线程更好的机会

```cpp
// mutex.cpp
bool first_wait = true;
while (true) {
    // ... 尝试获取锁 ...
    butex->Wait(generation, nullptr, !first_wait);  // 第一次FIFO，后续LIFO
    first_wait = false;
}
```

3. **双向链表 + in_queue 标志**：
   - 使用 `std::mutex` 保护的双向链表替代 Treiber 栈
   - 添加 `in_queue` 原子标志，防止重复移除导致的竞态条件

### 2. 快速创建/销毁场景

**问题描述：**
- 连续创建/销毁 1000 个 bthread 偶发卡住

**可能原因：**
- 任务池回收时可能存在未发现的竞态条件
- Context 复用时可能存在边界条件

**临时解决方案：**
- 使用 bthread 池而不是频繁创建/销毁

## 性能数据

修复后的基准测试结果：

```
[Benchmark 1] Create/Join Throughput
  Threads: 100, Iterations: 100
  Total time: ~16 ms
  Throughput: ~600,000 ops/sec
  Latency: ~1.6 us/op

[Benchmark 2] Yield Performance
  Threads: 4, Yields per thread: 10000
  Total time: ~0.6 ms
  Throughput: ~70,000,000 yields/sec
  Latency: ~14 ns/yield
```

## 参考资料

- [Butex 实现原理](./architecture/butex.md)
- [任务调度机制](./architecture/scheduler.md)
- [Generation 机制详解](./architecture/generation.md)

## 版本历史

- 2026-03-30 (第二轮): 实现 FIFO/LIFO 混合策略，解决极端高并发 mutex 卡死问题
- 2026-03-30 (第一轮): 修复主要卡死问题，分析极端高并发公平性问题
- 2026-03-25: 初始实现

---

## 附录：关键学习点

### A. 调试方法论

```
┌────────────────────────────────────────────────────────────┐
│                   并发问题调试流程                           │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  1. 复现问题                                                │
│     └→ 确定最小复现条件（迭代次数、线程数）                   │
│                                                            │
│  2. 分类问题                                                │
│     └→ 死锁？活锁？竞态条件？性能问题？                       │
│                                                            │
│  3. 定位范围                                                │
│     └→ 添加日志、计数器、断言                               │
│                                                            │
│  4. 提出假设                                                │
│     └→ 基于证据形成假设，而非猜测                            │
│                                                            │
│  5. 验证假设                                                │
│     └→ 最小化修改，一次只改一个变量                          │
│                                                            │
│  6. 如果失败，回退                                          │
│     └→ 保存失败尝试的记录，避免重复                          │
│                                                            │
│  7. 寻找参考                                                │
│     └→ 查看成熟实现如何解决类似问题                          │
│                                                            │
│  8. 文档化                                                  │
│     └→ 记录问题、原因、解决方案、经验教训                    │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### B. 本案例的关键教训

1. **不要急于实现自己的解决方案**
   - 先研究成熟的实现（官方 bthread）
   - 理解为什么他们这样设计
   - 本案例：官方的 `queue_lifo` 模式是经过实践验证的

2. **失败方案也有价值**
   - 记录每次尝试的假设、代码、结果、原因
   - 避免重复尝试相同的错误方案
   - 帮助理解问题的本质

3. **公平性问题在高并发下会被放大**
   - 小规模测试可能通过
   - 需要压力测试才能暴露
   - 设计时要考虑极端情况

4. **M:N 线程模型的特殊性**
   - 用户态线程调度与内核态不同
   - 上下文切换时机不同导致竞争特性不同
   - 需要特殊的公平性补偿机制

### C. 相关技术点

| 技术点 | 说明 | 参考 |
|--------|------|------|
| Treiber 栈 | 无锁 LIFO 栈 | 简单高效，但不支持 FIFO |
| 双向链表 | 支持 FIFO 和 LIFO | 需要锁保护，性能略低 |
| Generation 机制 | 检测状态变化 | 避免错过唤醒信号 |
| Memory Ordering | 原子操作的可见性 | acquire/release 语义 |
| Futex | 快速用户态互斥锁 | Linux 系统调用 |

### D. 推荐阅读

1. **官方 bthread 源码**：`benchmark/bthread/mutex.cpp`
2. **Linux futex 实现**：理解内核如何支持用户态锁
3. **无锁数据结构**：Treiber 栈、Michael-Scott 队列
4. **并发控制理论**：锁的实现、公平性、优先级反转

---

## 版本历史（续）

### 2026-04-02: Worker Shutdown 挂起问题

**问题描述：**

程序退出时调用 `bthread_shutdown()` 偶发性挂起（~10% 失败率），部分 worker 线程无法正确退出。

**现象：**
- 测试通过，但 shutdown 时卡住
- 部分 worker 打印 "Exiting"，部分没有
- 问题在 Windows 平台使用 `WaitOnAddress`/`WakeByAddressSingle` 时出现

**调试过程：**

#### 尝试 1：增加 WakeUp 次数和延迟

```cpp
void Scheduler::Shutdown() {
    running_.store(false, ...);
    for (int i = 0; i < 10; ++i) {
        WakeAllWorkers();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Join workers...
}
```

**结果：** 失败率仍约 10%

**分析：** 问题不是唤醒遗漏，而是 worker 对 running 状态的检查存在竞态

#### 尝试 2：分离 running 标志和 sleep_token

原有实现：
```cpp
// Worker::Run()
while (Scheduler::Instance().running()) { ... }

// Worker::WaitForTask()
platform::FutexWait(&sleep_token_, expected, &ts);
```

**问题：** `running` 标志和 `sleep_token` 是独立的，可能存在以下竞态：

```
时间线:
  Worker                           Scheduler
  --------                         ----------
  检查 running=true
                                   running=false
                                   WakeAllWorkers()
                                   (worker 不在等待，唤醒无效)
  FutexWait()  -- 永久阻塞
```

#### 尝试 3：参考官方 ParkingLot 实现

查看 `benchmark/official_bthread_reference/parking_lot.h`：

```cpp
class ParkingLot {
    // higher 31 bits for signalling, LSB for stopping.
    butil::atomic<int> _pending_signal;

    void stop() {
        _pending_signal.fetch_or(1);  // 设置 LSB
        futex_wake_private(&_pending_signal, 10000);
    }

    bool stopped() const { return val & 1; }
};
```

**关键洞察：**
- 停止标志和等待 token 合并在同一个原子变量中
- LSB（最低位）作为停止标志
- 高 31 位作为唤醒计数器

#### 尝试 4：实现停止标志在 sleep_token 中

```cpp
// include/bthread/worker.h
std::atomic<int> sleep_token_{0};
static constexpr int STOP_FLAG = 1;

bool IsStopped() const {
    return sleep_token_.load(std::memory_order_acquire) & STOP_FLAG;
}

void Stop() {
    sleep_token_.fetch_or(STOP_FLAG, std::memory_order_release);
    platform::FutexWake(&sleep_token_, 1);
}

void WakeUp() {
    sleep_token_.fetch_add(2, std::memory_order_release);  // 加 2，跳过 stop bit
    platform::FutexWake(&sleep_token_, 1);
}

void WaitForTask() {
    int token = sleep_token_.load(std::memory_order_acquire);
    if (token & STOP_FLAG) return;
    // ...
    platform::FutexWait(&sleep_token_, token, &ts);
}
```

**结果：** 成功率从 ~0% 提升到 ~90%

**遗留问题：** 仍有 ~10% 失败率

#### 深入分析：Windows WaitOnAddress 的行为

```cpp
// platform_windows.cpp
int FutexWait(std::atomic<int>* addr, int expected, const timespec* timeout) {
    DWORD ms = timeout ? ... : INFINITE;
    BOOL ok = WaitOnAddress(static_cast<VOID*>(addr), &expected, sizeof(int), ms);
    // ...
}
```

**潜在问题：**
1. `WaitOnAddress` 比较 `addr` 和 `expected`
2. 如果值已经改变，应该立即返回
3. 但在多线程环境下，可能存在时序窗口

**调试输出分析：**

```
[Worker 0] WaitForTask: waiting (token=40)
[Worker 0] WaitForTask: woke up (old_token=40, new_token=41, stopped=1)
[Worker 5] WaitForTask: waiting (token=0)
[Worker 5] WaitForTask: woke up (old_token=0, new_token=41, stopped=1)
```

有时 worker 在 token=0 时开始等待（说明它之前没有被唤醒过），这表明唤醒信号可能丢失。

#### 当前状态

**成功率：** ~90%

**已知问题场景：**
1. Worker 在执行任务时，Stop() 被调用
2. Stop() 设置标志并唤醒，但 worker 不在 FutexWait 中
3. Worker 完成任务后重新进入循环，检查 IsStopped()
4. 但由于某种原因，检查失败或 worker 又进入了 WaitForTask

**待调查方向：**
1. 是否需要在任务执行后也检查 IsStopped()？
2. Windows WaitOnAddress 的唤醒语义是否完全正确？
3. 是否需要类似官方的 interrupt_pthread 机制？

**临时解决方案：**
- 多次调用 Stop() 并添加延迟
- 在 Worker::Run() 任务执行后检查 IsStopped()

```cpp
void Worker::Run() {
    while (!IsStopped()) {
        TaskMetaBase* task = PickTask();
        if (task == nullptr) {
            if (IsStopped()) break;
            WaitForTask();
            if (IsStopped()) break;
            continue;
        }
        // ... 执行任务 ...
        if (IsStopped()) {
            HandleTaskAfterRun(completed_task);
            break;
        }
        HandleTaskAfterRun(completed_task);
    }
}
```

**经验教训：**

1. **原子状态和等待对象应该合并**
   - 分离的 `running` 和 `sleep_token` 导致竞态
   - 参考官方 ParkingLot 的设计

2. **Windows WaitOnAddress 的特殊行为**
   - 需要确保值比较在原子读取之后
   - 可能需要额外同步

3. **检查停止标志的时机**
   - 不仅在循环开始检查
   - 任务执行后也应检查

4. **多线程关闭的复杂性**
   - 官方 bthread 使用 `pthread_kill` 发送信号打断阻塞
   - Windows 可能需要不同机制