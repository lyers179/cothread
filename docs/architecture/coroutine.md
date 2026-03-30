# 协程池架构

## 概述

本项目实现了一个基于 C++20 协程的 M:N 线程池，与现有 bthread 并存。协程池提供现代的 async/await 风格 API，使用无栈协程（stackless coroutine）实现。

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                      User API Layer                          │
│  Task<T>, SafeTask<T>, co_spawn(), co_await, yield(), sleep │
├─────────────────────────────────────────────────────────────┤
│                   Coroutine Scheduler                        │
│  CoroutineMeta Pool, FramePool, CoroutineQueue, Timer       │
├─────────────────────────────────────────────────────────────┤
│                   Worker Threads (Dedicated)                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  │ Worker 3 │   │
│  │          │  │          │  │          │  │          │   │
│  │  Loop:   │  │  Loop:   │  │  Loop:   │  │  Loop:   │   │
│  │  Pop()   │  │  Pop()   │  │  Pop()   │  │  Pop()   │   │
│  │  Resume()│  │  Resume()│  │  Resume()│  │  Resume()│   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└─────────────────────────────────────────────────────────────┘
```

**注意：** Phase 1 使用专用协程 Worker 线程，与 bthread 的 Worker 池独立。未来版本可能统一调度。

## 核心组件

### 1. Task<T>（协程返回类型）

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

**使用示例：**

```cpp
coro::Task<int> compute_value() {
    co_return 42;
}

// 方式1：直接恢复
auto task = compute_value();
task.handle().resume();
int result = task.get();  // result = 42

// 方式2：通过调度器
auto task = coro::co_spawn(compute_value());
// ... 等待完成 ...
int result = task.get();
```

### 2. SafeTask<T>（安全协程返回类型）

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

**使用示例：**

```cpp
coro::SafeTask<int> safe_compute() {
    co_return 42;  // 成功
}

coro::SafeTask<int> safe_error() {
    co_return coro::Error(1, "failed");  // 错误
}

auto r1 = safe_compute().handle().resume(), safe_compute().get();
if (r1.is_ok()) {
    std::cout << r1.value() << std::endl;  // 42
}

auto r2 = safe_error().handle().resume(), safe_error().get();
if (r2.is_err()) {
    std::cout << r2.error().message() << std::endl;  // "failed"
}
```

### 3. CoroutineMeta（协程元数据）

```cpp
struct CoroutineMeta {
    enum State : uint8_t {
        READY,      // 就绪，等待执行
        RUNNING,    // 正在执行
        SUSPENDED,  // 挂起（等待锁、条件变量、sleep）
        FINISHED    // 已完成
    };

    std::coroutine_handle<> handle;         // 协程句柄
    std::atomic<State> state;               // 状态
    bthread::Worker* owner_worker;          // 所属 Worker
    std::atomic<bool> cancel_requested;     // 取消请求
    void* waiting_sync;                     // 等待的同步原语
    CoroutineMeta* next;                    // 队列链接
    uint32_t slot_index;                    // 槽位索引
    uint32_t generation;                    // 代际
};
```

### 4. CoroutineScheduler（协程调度器）

```cpp
class CoroutineScheduler {
public:
    static CoroutineScheduler& Instance();

    void Init();                            // 初始化（启动 Worker）
    void Shutdown();                        // 关闭

    template<typename T>
    Task<T> Spawn(Task<T> task);            // 生成协程

    void EnqueueCoroutine(CoroutineMeta* meta);  // 入队协程
    CoroutineMeta* AllocMeta();             // 分配元数据
    void FreeMeta(CoroutineMeta* meta);     // 释放元数据

private:
    std::vector<std::thread> workers_;      // Worker 线程
    CoroutineQueue global_queue_;           // 全局队列
    std::vector<std::unique_ptr<CoroutineMeta>> meta_pool_;  // 元数据池
};
```

### 5. FramePool（帧内存池）

```cpp
class FramePool {
public:
    void Init(size_t block_size, size_t initial_count);
    void* Allocate(size_t size);            // 分配帧
    void Deallocate(void* block);           // 释放帧
    size_t block_size() const;
};
```

**内存布局：**

```
FramePool (8KB blocks)
┌─────────────────────────────────────────────────────────┐
│ Block 0  │ Block 1  │ Block 2  │ ...  │ Block N   │
│ 8KB      │ 8KB      │ 8KB      │      │ 8KB       │
└─────────────────────────────────────────────────────────┘
     ↓
  Free List (intrusive)
  ┌─────────┐    ┌─────────┐    ┌─────────┐
  │ FreeNode│───>│ FreeNode│───>│ FreeNode│───> nullptr
  └─────────┘    └─────────┘    └─────────┘
```

### 6. CoroutineQueue（协程队列）

MPSC（多生产者单消费者）队列：

```cpp
class CoroutineQueue {
public:
    void Push(CoroutineMeta* meta);         // 多生产者安全
    CoroutineMeta* Pop();                   // 单消费者
    bool Empty() const;

private:
    std::atomic<CoroutineMeta*> head_;      // 头指针
    std::atomic<CoroutineMeta*> tail_;      // 尾指针
};
```

## 同步原语

### CoMutex（协程互斥锁）

```cpp
class CoMutex {
public:
    LockAwaiter lock();     // co_await mutex.lock()
    bool try_lock();        // 非阻塞尝试
    void unlock();          // 解锁
};
```

**实现原理：**

```
状态位：
  LOCKED = 1       - 锁已被持有
  HAS_WAITERS = 2  - 有等待者

锁获取流程：
  co_await mutex.lock()
      │
      ├─> try_lock() 成功 ──> 获得锁
      │
      └─> try_lock() 失败
              │
              ├─> 设置 HAS_WAITERS
              ├─> Push 到 waiters_ 队列
              ├─> 挂起协程
              └─> 被唤醒后拥有锁

锁释放流程：
  mutex.unlock()
      │
      ├─> Pop 等待者
      │       │
      │       ├─> 有等待者 ──> 直接转移锁所有权
      │       │
      │       └─> 无等待者 ──> 清除 LOCKED
      │
      └─> 唤醒等待者（入队调度）
```

### CoCond（协程条件变量）

```cpp
class CoCond {
public:
    WaitAwaiter wait(CoMutex& mutex);   // co_await cond.wait(mutex)
    void signal();                      // 唤醒一个
    void broadcast();                   // 唤醒全部
};
```

**使用示例：**

```cpp
coro::Task<void> producer(CoMutex& m, CoCond& c, int& data) {
    co_await m.lock();
    data = 42;
    c.signal();
    m.unlock();
}

coro::Task<void> consumer(CoMutex& m, CoCond& c, int& data) {
    co_await m.lock();
    while (data == 0) {
        co_await c.wait(m);
    }
    assert(data == 42);
    m.unlock();
}
```

## 取消机制

```cpp
class CancelSource {
public:
    CancellationToken token();   // 获取 token
    void cancel();               // 请求取消
    void reset();                // 重置状态
};

class CancellationToken {
public:
    bool is_cancelled() const;                   // 检查取消状态
    CheckCancelAwaiter check_cancel();           // co_await 检查点
};
```

**使用示例：**

```cpp
coro::Task<int> cancelable_work(CoroutineToken& token) {
    for (int i = 0; i < 1000; ++i) {
        if (co_await token.check_cancel()) {
            co_return -1;  // 被取消
        }
        // ... 执行工作 ...
        co_await coro::yield();
    }
    co_return 0;  // 正常完成
}

coro::CancelSource source;
auto task = coro::co_spawn(cancelable_work(source.token()));
// ... 一段时间后 ...
source.cancel();  // 请求取消
```

## 控制函数

### yield()

显式让出执行权：

```cpp
coro::Task<void> cooperative_work() {
    for (int i = 0; i < 10; ++i) {
        std::cout << i << std::endl;
        co_await coro::yield();  // 让其他协程执行
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

**Timer 实现：**

```
Sleep Thread:
┌─────────────────────────────────────────────────────┐
│  sleep_queue_: map<time_point, CoroutineMeta*>     │
│                                                     │
│  Loop:                                              │
│    1. 检查最近唤醒时间                               │
│    2. 等待到该时间或被通知                           │
│    3. 唤醒所有到期协程                               │
│       └─> EnqueueCoroutine(meta)                    │
└─────────────────────────────────────────────────────┘
```

## 协程生命周期

```
         ┌──────────────────────────────────────────────────────┐
         │                     co_spawn()                        │
         └────────────────────────┬─────────────────────────────┘
                                  ▼
         ┌──────────────────────────────────────────────────────┐
         │  1. AllocMeta() - 从池中分配 CoroutineMeta           │
         │  2. FramePool::Allocate() - 分配协程帧               │
         │  3. 初始化 handle, state=READY                       │
         │  4. EnqueueCoroutine() - 入队调度                    │
         └────────────────────────┬─────────────────────────────┘
                                  ▼
         ┌──────────────────────────────────────────────────────┐
         │               Worker::Resume()                        │
         │  current_coro_meta_ = meta                           │
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
      │ FreeMeta()    │   │               │   │ 唤醒等待者    │
      └───────────────┘   └───────────────┘   └───────────────┘
```

## 文件结构

```
include/coro/
  ├── coroutine.h      # Task<T>, SafeTask<T>, yield(), sleep()
  ├── scheduler.h      # CoroutineScheduler, co_spawn
  ├── meta.h           # CoroutineMeta, CoroutineQueue
  ├── mutex.h          # CoMutex
  ├── cond.h           # CoCond
  ├── cancel.h         # CancellationToken, CancelSource
  ├── result.h         # Result<T>, Error
  └── frame_pool.h     # FramePool

src/coro/
  ├── coroutine.cpp    # Task/SafeTask 实现
  ├── scheduler.cpp    # 调度器 + Timer 实现
  ├── mutex.cpp        # CoMutex 实现
  ├── cond.cpp         # CoCond 实现
  ├── cancel.cpp       # 取消机制实现
  └── frame_pool.cpp   # 帧池实现
```

## 与 bthread 的关系

| 特性 | bthread | 协程池 |
|------|---------|--------|
| 实现 | 有栈协程 | 无栈协程 (C++20) |
| API 风格 | C 函数式 | async/await |
| Worker | 共享 | 专用 (Phase 1) |
| 栈管理 | 独立栈 | 帧池 |
| 同步原语 | Butex | CoMutex/CoCond |

**设计决策：** Phase 1 保持独立实现，简化初始开发。未来可考虑统一调度器。

## 已知限制

1. **嵌套 co_spawn** - `co_await co_spawn(inner())` 可能死锁
2. **分离协程** - 内存管理待完善
3. **关闭竞态** - 程序退出时可能有竞态条件
4. **并发压力** - 高并发 Mutex 操作可能挂起

详见 [COROUTINE_TODO.md](../COROUTINE_TODO.md)。

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
void CoroutineScheduler::CoroutineWorkerLoop() {
    while (running_) {
        CoroutineMeta* meta = global_queue_.Pop();
        if (meta) {
            CORO_LOG("Worker resuming coroutine %p, state=%d",
                     meta, meta->state.load());
            // ...
        }
    }
}
```