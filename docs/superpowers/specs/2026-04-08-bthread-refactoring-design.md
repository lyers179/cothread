---
name: bthread-code-refactoring
description: 全项目代码质量重构设计文档
type: project
created: 2026-04-08
---

# Bthread 代码质量重构设计

## 概述

本设计文档描述 bthread M:N 线程库的全面代码质量重构方案。

**目标**：改善命名/组织、消除代码重复、优化模块划分

**范围**：全项目范围

**策略**：全面重构

**约束**：无性能约束、无时间约束

---

## 第一部分：目录结构重组

### 当前问题

- `include/bthread/` 目录混乱：核心文件和辅助文件混在一起
- `.h` 和 `.hpp` 后缀混用，语义不清晰
- `butex.h`、`global_queue.h`、`work_stealing_queue.h` 没有明确归类

### 目标结构

```
include/bthread/
├── core/                    # 核心抽象层
│   ├── task_meta.hpp        # TaskMeta 定义
│   ├── task_meta_base.hpp   # 基类（已有，保留）
│   ├── task.hpp             # Task handle（已有，保留）
│   ├── scheduler.hpp        # 调度器（已有，保留）
│   ├── worker.hpp           # Worker（已有，保留）
│   └── task_group.hpp       # TaskGroup（已有，保留）
│
├── sync/                    # 同步原语层
│   ├── mutex.hpp            # Mutex（已有）
│   ├── cond.hpp             # CondVar（已有）
│   ├── event.hpp            # Event（已有）
│   ├── butex.hpp            # Butex（移动进来）
│   └── waiter.hpp           # WaiterNode/WaitState 统一定义
│
├── queue/                   # 队列组件层
│   ├── global_queue.hpp     # 全局队列（移动进来）
│   ├── work_stealing_queue.hpp  # 窃取队列（移动进来）
│   └── mpsc_queue.hpp       # MPSC 队列模板（新增）
│
├── platform/                # 平台抽象层（已有，保留）
│   ├── context.hpp
│   ├── stack.hpp
│   ├── futex.hpp
│   └── platform.hpp
│
├── api/                     # 用户 API 层
│   ├── spawn.hpp            # spawn 函数
│   ├── config.hpp           # 配置函数
│   └── once.hpp             # bthread_once
│
├── detail/                  # 内部实现细节
│   ├── entry.hpp            # 入口函数包装
│   └── timer.hpp            # TimerThread 内部
│
└── bthread.hpp              # 主入口（统一 include）

include/coro/                # 协程模块（保留现有结构）
├── coroutine.h
├── meta.h
├── result.h
├── cancel.h
├── frame_pool.h
└── scheduler.h

include/bthread.h            # C API 入口（保持兼容）
```

### 命名规范

- `.hpp` = C++ 头文件，包含模板、类定义
- `.h` = C 兼容头文件（只有 `bthread.h`）

---

## 第二部分：接口统一与代码去重

### 发现的重复问题

| 问题 | 描述 | 影响 |
|------|------|------|
| MPSC 队列重复 | `GlobalQueue` 和 `coro::CoroutineQueue` 实现完全相同 | 代码重复 ~60 行 |
| WaiterNode 命名冲突 | `task_meta.h` 和 `mutex.hpp` 都有 `WaiterNode`，用途不同 | 易混淆 |
| WaiterState 冘余 | `WaiterState` 和 `is_waiting` 功能重叠 | 内存浪费 |
| 同步原语分离 | Mutex 对 bthread/coroutine 用不同等待队列 | 逻辑重复 |
| 后缀不一致 | `.h` 和 `.hpp` 混用，无语义区分 | 难识别 |

### 统一方案

#### 1. 统一 MPSC 队列模板

```cpp
// include/bthread/queue/mpsc_queue.hpp
namespace bthread {

template<typename T>
class MpscQueue {
    void Push(T* item);
    T* Pop();
    bool Empty() const;
};

// 特化别名
using TaskQueue = MpscQueue<TaskMetaBase>;
using CoroutineQueue = MpscQueue<coro::CoroutineMeta>;

} // namespace bthread
```

- 删除 `coro::CoroutineQueue`，使用 `bthread::TaskQueue`
- `GlobalQueue` 变为 `TaskQueue` 的别名

#### 2. 重命名 WaiterNode 消除歧义

```cpp
// task_meta.hpp 中：
struct ButexWaiterNode { ... };  // 原 WaiterNode，用于 Butex

// mutex.hpp 中：
struct MutexWaiterNode { ... };  // 原 WaiterNode，用于 Mutex
```

#### 3. 简化等待状态

```cpp
// TaskMeta 中简化：
struct WaitState {
    std::atomic<bool> is_waiting{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> wakeup{false};
    int64_t deadline_us{0};
    int timer_id{0};
    // 移除 prev/next，使用 ButexWaiterNode 的内置链表
};
```

#### 4. 统一同步原语等待队列

```cpp
// Mutex 使用 TaskQueue 而非单独的 MutexWaiterNode 链表
class Mutex {
    TaskQueue coroutine_waiters_;  // 统一使用 TaskQueue
    Butex* bthread_butex_;         // bthread 继续用 Butex
};
```

---

## 第三部分：命名规范统一

### 文件重命名计划

| 旧名称 | 新名称 |
|--------|--------|
| `task_meta.h` | `core/task_meta.hpp` |
| `butex.h` | `sync/butex.hpp` |
| `global_queue.h` | `queue/global_queue.hpp` |
| `work_stealing_queue.h` | `queue/work_stealing_queue.hpp` |
| `task_group.h` | `core/task_group.hpp` |
| `timer_thread.h` | `detail/timer.hpp` |
| `worker.h` | `core/worker.hpp` |
| `scheduler.h` | `core/scheduler.hpp`（合并） |
| `once.h` | `api/once.hpp` |
| `execution_queue.h` | `queue/execution_queue.hpp` |

### 内部命名风格

```cpp
// 类名：PascalCase
class TaskMeta {};
class WorkStealingQueue {};

// 函数名：snake_case
void enqueue_task();
void wake_idle_workers();

// 成员变量：snake_case_ 后缀
TaskMeta* current_task_;
std::atomic<int> wake_count_;

// 常量：UPPER_CASE
static constexpr int BATCH_SIZE = 8;

// 枚举：PascalCase，值 PascalCase
enum class TaskState { Ready, Running, Suspended, Finished };
```

---

## 第四部分：模块职责划分

### Core 层

```
core/
├── scheduler.hpp/.cpp     # 调度器：Init/Shutdown/Submit/EnqueueTask
├── worker.hpp/.cpp        # Worker 主循环：Run/PickTask/SuspendCurrent
├── worker_stealing.hpp/.cpp  # 任务窃取逻辑（从 worker.cpp 拆分）
├── worker_shutdown.hpp/.cpp  # shutdown drain 逻辑（从 worker.cpp 拆分）
├── task_meta.hpp/.cpp     # TaskMeta 定义 + resume 实现
├── task_group.hpp/.cpp    # TaskMeta 池管理：Alloc/Dealloc/GetSuspended
├── task_handle.hpp/.cpp   # Task 类实现（join/detach）
└── task_meta_base.hpp     # 基类定义（纯头文件）
```

### Sync 层

```
sync/
├── mutex.hpp/.cpp         # Mutex 公共接口：lock/unlock/try_lock
├── mutex_bthread.hpp/.cpp # bthread 锁路径：lock_bthread（拆分）
├── mutex_pthread.hpp/.cpp # pthread 锁路径：lock_pthread（拆分）
├── mutex_awaiter.hpp/.cpp # coroutine awaiter：LockAwaiter（拆分）
├── butex.hpp/.cpp         # Butex 公共接口：Wait/Wake
├── butex_queue.hpp/.cpp   # MPSC 等待队列：AddToHead/AddToTail/Pop（拆分）
├── cond.hpp/.cpp          # CondVar 实现
├── event.hpp/.cpp         # Event 实现
└── waiter.hpp             # WaiterNode/WaitState 定义（统一）
```

### Queue 层

```
queue/
├── global_queue.hpp/.cpp  # 全局任务队列
├── work_stealing_queue.hpp/.cpp  # 窃取队列
├── mpsc_queue.hpp         # MPSC 队列模板（统一实现）
└── execution_queue.hpp/.cpp  # ExecutionQueue（保持）
```

### Platform 层（保持）

```
platform/
├── platform.hpp/.cpp      # 平台抽象入口
├── platform_linux.hpp/.cpp
├── platform_windows.hpp/.cpp
├── context.hpp/.cpp       # 上下文切换
├── stack.hpp/.cpp         # 栈管理
└── futex.hpp/.cpp         # futex 操作
```

### API 层

```
api/
├── spawn.hpp/.cpp         # spawn 函数模板
├── config.hpp/.cpp        # set_worker_count/get_worker_count
├── once.hpp/.cpp          # bthread_once
└── timer.hpp/.cpp         # bthread_timer_add/cancel
```

### Detail 层

```
detail/
├── entry.hpp/.cpp         # BthreadEntry 包装函数
└── timer_thread.hpp/.cpp  # TimerThread 内部实现
```

### 文件大小目标

- 每个文件控制在 100-150 行
- 单一职责：一个文件只做一件事
- 头文件包含声明，cpp 包含实现细节

---

## 第五部分：实现计划

### 阶段 1：目录结构重组（低风险）

**改动**：
1. 创建新目录：`core/`、`sync/`、`queue/`、`api/`、`detail/`
2. 移动文件到新位置（不修改内容）
3. 更新 `#include` 路径
4. 统一 `.h` → `.hpp` 后缀

**验证**：编译通过 + 所有测试通过

**预计改动文件数**：20+

---

### 阶段 2：命名规范化（低风险）

**改动**：
1. 重命名 `WaiterNode` → `ButexWaiterNode` / `MutexWaiterNode`
2. 统一内部命名风格
3. 合并 `scheduler.h` 和 `scheduler.hpp` 为单一文件

**验证**：编译通过 + 所有测试通过

**预计改动文件数**：15+

---

### 阶段 3：MPSC 队列统一（中等风险）

**改动**：
1. 创建 `queue/mpsc_queue.hpp` 模板
2. `GlobalQueue` 使用 `MpscQueue<TaskMetaBase>`
3. 删除 `coro::CoroutineQueue`，使用统一模板
4. 更新所有引用点

**验证**：编译通过 + 所有测试通过 + 性能对比

**预计改动文件数**：8+

---

### 阶段 4：同步原语拆分（中等风险）

**改动**：
1. `mutex.cpp` 拆分为 4 个文件
2. `butex.cpp` 拆分为 2 个文件（接口 + 队列）
3. 创建 `sync/waiter.hpp` 统一等待状态定义

**验证**：编译通过 + 所有测试通过 + Mutex 压力测试

**预计改动文件数**：12+

---

### 阶段 5：Worker 拆分（中等风险）

**改动**：
1. `worker.cpp` 拆分为 3 个文件（主循环 + 窃取 + shutdown）
2. 独立测试各模块

**验证**：编译通过 + 所有测试通过 + Worker 批量测试

**预计改动文件数**：10+

---

### 阶段 6：API 整理（低风险）

**改动**：
1. 拆分 `api.hpp` 为 `spawn.hpp`、`config.hpp`
2. 移动 `once.h`、`timer_thread.h` 到合适位置
3. 更新主入口 `bthread.hpp`

**验证**：编译通过 + 所有测试通过 + Demo 程序验证

**预计改动文件数**：8+

---

### 总体顺序

```
阶段1 → 阶段2 → 阶段3 → 阶段4 → 阶段5 → 阶段6
  ↓        ↓        ↓        ↓        ↓        ↓
编译测试  编译测试  编译测试  编译测试  编译测试  编译测试
```

每阶段完成后：
1. 运行完整测试套件
2. 提交阶段性改动
3. 用户确认后进入下一阶段

---

## 成功标准

1. 所有测试通过
2. 目录结构清晰，职责分明
3. 无代码重复（MPSC 队列统一）
4. 命名规范一致
5. 文件大小适中（100-150行）
6. 新开发者易于理解项目结构