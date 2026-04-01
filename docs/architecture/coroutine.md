# 协程池架构

## 概述

本项目实现了一个基于 C++20 协程的 M:N 线程池，与现有 bthread 共享统一的调度器。协程池提供现代的 async/await 风格 API，使用无栈协程（stackless coroutine）实现。

## 与 bthread 的统一架构

```
┌─────────────────────────────────────────────────────────────┐
│                      User API Layer                          │
│  Task<T>, SafeTask<T>, co_spawn(), co_await, yield(), sleep │
├─────────────────────────────────────────────────────────────┤
│               Unified Scheduler (bthread::Scheduler)        │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                   Global Queue                       │   │
│  │  [TaskMetaBase*] -> [TaskMetaBase*] -> ...          │   │
│  └─────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│                 Unified Worker Threads                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  │ Worker N │   │
│  │          │  │          │  │          │  │          │   │
│  │ Handles: │  │ Handles: │  │ Handles: │  │ Handles: │   │
│  │ - bthread│  │ - bthread│  │ - bthread│  │ - bthread│   │
│  │ - coro   │  │ - coro   │  │ - coro   │  │ - coro   │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└─────────────────────────────────────────────────────────────┘
```

**关键变化：** 协程现在与 bthread 共享 Worker 线程池，不再使用专用协程 Worker。

## 核心组件

### 1. CoroutineMeta（协程元数据）

继承自 `TaskMetaBase`，与 `TaskMeta` 共享统一接口。

```cpp
// include/coro/meta.h
struct CoroutineMeta : bthread::TaskMetaBase {
    // 构造函数 - 设置类型为 COROUTINE
    CoroutineMeta() : TaskMetaBase() {
        type = bthread::TaskType::COROUTINE;
    }

    // ========== 向后兼容的 State 枚举 ==========
    // 映射到 bthread::TaskState
    enum State : uint8_t {
        READY = static_cast<uint8_t>(bthread::TaskState::READY),
        RUNNING = static_cast<uint8_t>(bthread::TaskState::RUNNING),
        SUSPENDED = static_cast<uint8_t>(bthread::TaskState::SUSPENDED),
        FINISHED = static_cast<uint8_t>(bthread::TaskState::FINISHED)
    };

    // ========== 协程句柄（协程特有） ==========
    std::coroutine_handle<> handle;

    // ========== 取消支持（协程特有） ==========
    std::atomic<bool> cancel_requested{false};

    // ========== 恢复执行 ==========
    void resume() override {
        if (handle && !handle.done()) {
            handle.resume();
        }
    }
};
```

**继承关系：**

```
TaskMetaBase (统一基类)
    │
    ├── TaskMeta (bthread 任务)
    │   - stack, context, fn, arg
    │   - resume() 通过 SwapContext
    │
    └── CoroutineMeta (协程任务)
        - handle
        - resume() 通过 handle.resume()
```

### 2. Task<T>（协程返回类型）

异常风格的协程返回类型，通过 `co_return` 返回值。

```cpp
template<typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;

    // 获取协程句柄
    std::coroutine_handle<promise_type> handle() const;

    // 检查协程是否完成
    bool is_done() const;

    // 获取结果（阻塞，异常时抛出）
    T get();

    // Awaiter 接口
    bool await_ready();
    void await_suspend(std::coroutine_handle<> awaiting);
    T await_resume();
};
```

### 3. SafeTask<T>（安全协程返回类型）

Result<T> 风格的协程返回类型，不抛出异常。

```cpp
template<typename T>
class SafeTask {
public:
    using promise_type = SafeTaskPromise<T>;

    // 获取结果（返回 Result<T>，不抛出）
    coro::Result<T> get();

    // Awaiter 接口
    coro::Result<T> await_resume();
};
```

### 4. 协程调度

协程通过统一调度器提交执行：

```cpp
// 方式1：使用 bthread::Scheduler
bthread::Scheduler::Instance().Spawn(task);

// 方式2：使用 coro::co_spawn（委托给 Scheduler）
auto task = coro::co_spawn(my_coroutine());
```

**调度流程：**

```
co_spawn(task)
    │
    ├─> 分配 CoroutineMeta
    │
    ├─> meta->handle = task.handle()
    │
    ├─> meta->state = READY
    │
    └─> Scheduler::Submit(meta)
            │
            └─> Worker 执行
                    │
                    ├─> 检查 type == COROUTINE
                    │
                    └─> RunCoroutine(meta)
                            │
                            └─> meta->handle.resume()
```

## 统一同步原语

协程使用统一的 `bthread::Mutex` 和 `bthread::CondVar`，与 bthread 完全兼容：

### Mutex（统一互斥锁）

```cpp
bthread::Mutex mutex;

// 从协程
co_await mutex.lock_async();

// 从 bthread
mutex.lock();
```

### CondVar（统一条件变量）

```cpp
bthread::CondVar cond;
bthread::Mutex mutex;

// 从协程
co_await cond.wait_async(mutex);

// 从 bthread
cond.wait(mutex);
```

### Event（统一事件）

```cpp
bthread::Event event(false);

// 从协程
co_await event.wait_async();

// 从 bthread
event.wait();
```

## 控制函数

### yield()

显式让出执行权：

```cpp
coro::Task<void> cooperative_work() {
    for (int i = 0; i < 10; ++i) {
        std::cout << i << std::endl;
        co_await coro::yield();  // 让其他任务执行
    }
}
```

### sleep()

定时挂起：

```cpp
coro::Task<void> delayed_work() {
    std::cout << "Start" << std::endl;
    co_await coro::sleep(std::chrono::milliseconds(1000));
    std::cout << "After 1 second" << std::endl;
}
```

## 协程生命周期

```
         ┌──────────────────────────────────────────────────────┐
         │                     co_spawn()                        │
         └────────────────────────┬─────────────────────────────┘
                                  ▼
         ┌──────────────────────────────────────────────────────┐
         │  1. 分配 CoroutineMeta                                │
         │  2. meta->handle = task.handle()                     │
         │  3. meta->state = READY                               │
         │  4. Scheduler::Submit(meta)                          │
         └────────────────────────┬─────────────────────────────┘
                                  ▼
         ┌──────────────────────────────────────────────────────┐
         │               Worker::RunCoroutine()                  │
         │  current_task_ = meta                                │
         │  state = RUNNING                                      │
         │  handle.resume()                                      │
         └────────────────────────┬─────────────────────────────┘
                                  ▼
              ┌───────────────────┼───────────────────┐
              ▼                   ▼                   ▼
      ┌───────────────┐   ┌───────────────┐   ┌───────────────┐
      │  co_return    │   │  co_await     │   │  异常         │
      │  完成         │   │  挂起         │   │  未处理       │
      └───────┬───────┘   └───────┬───────┘   └───────┬───────┘
              ▼                   ▼                   ▼
      ┌───────────────┐   ┌───────────────┐   ┌───────────────┐
      │ state=FINISHED│   │ state=SUSPENDED│  │ 存储异常      │
      │ 唤醒等待者    │   │ 等待唤醒      │   │ state=FINISHED│
      │ 释放资源      │   │               │   │ 唤醒等待者    │
      └───────────────┘   └───────────────┘   └───────────────┘
```

## 文件结构

```
include/coro/
  ├── coroutine.h      # Task<T>, SafeTask<T>, yield(), sleep()
  ├── scheduler.h      # co_spawn, 委托给 bthread::Scheduler
  ├── meta.h           # CoroutineMeta : TaskMetaBase
  ├── cancel.h         # CancellationToken, CancelSource
  ├── result.h         # Result<T>, Error
  └── frame_pool.h     # FramePool

include/bthread/sync/
  ├── mutex.hpp        # 统一互斥锁 (lock() + lock_async())
  ├── cond.hpp         # 统一条件变量 (wait() + wait_async())
  └── event.hpp        # 统一事件

src/coro/
  ├── coroutine.cpp    # yield(), sleep() 实现
  ├── scheduler.cpp    # Timer 线程，委托给 Scheduler
  ├── cancel.cpp       # 取消机制实现
  └── frame_pool.cpp   # 帧池实现

src/bthread/sync/
  ├── mutex.cpp        # 统一 Mutex 实现
  ├── cond.cpp         # 统一 CondVar 实现
  └── event.cpp        # 统一 Event 实现
```

## 与 bthread 的关系

| 特性 | bthread | 协程 |
|------|---------|------|
| 基类 | TaskMeta : TaskMetaBase | CoroutineMeta : TaskMetaBase |
| 实现 | 有栈协程（汇编） | 无栈协程（C++20） |
| API 风格 | C 函数式 | async/await |
| Worker | 共享统一 Worker 池 | 共享统一 Worker 池 |
| 调度器 | bthread::Scheduler | bthread::Scheduler |
| 同步原语 | Mutex, CondVar | Mutex::lock_async(), CondVar::wait_async() |

## 迁移指南

### 从独立 CoroutineScheduler 迁移

```cpp
// 旧代码
coro::CoroutineScheduler::Instance().Init();
coro::CoroutineScheduler::Instance().Spawn(task);

// 新代码
bthread::Scheduler::Instance().Init();
bthread::Scheduler::Instance().Spawn(task);
// 或者
coro::co_spawn(task);  // 自动使用统一调度器
```

## 已知限制

1. **嵌套 co_spawn** - `co_await co_spawn(inner())` 可能死锁
2. **分离协程** - 内存管理待完善
3. **关闭竞态** - 程序退出时可能有竞态条件

## 调试技巧

### 启用日志

```cpp
// 在 scheduler.cpp 中
#define CORO_DEBUG 1

// 日志输出
CORO_LOG("Coroutine %p state: %d", meta, meta->state.load());
```

### 检查死锁

```cpp
// 添加超时
auto result = co_await mutex.lock_with_timeout(std::chrono::seconds(5));
if (!result) {
    std::cerr << "Possible deadlock" << std::endl;
}
```

### 状态追踪

```cpp
// 检查任务类型
if (task->type == bthread::TaskType::COROUTINE) {
    auto* meta = static_cast<coro::CoroutineMeta*>(task);
    CORO_LOG("Coroutine handle: %p, done: %d",
             meta->handle.address(), meta->handle.done());
}
```