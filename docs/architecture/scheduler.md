# 统一调度器架构

## 概述

bthread 现在使用统一的调度器架构，将传统的 bthread（汇编上下文切换）和 C++20 协程整合到同一个调度系统中。两种任务类型共享相同的 Worker 线程池和全局队列。

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                      User Application                        │
├─────────────────────────────────────────────────────────────┤
│  Modern C++ API (spawn, Task)  │  Legacy C API (bthread_*)  │
├─────────────────────────────────────────────────────────────┤
│          Unified Scheduler (bthread::Scheduler)             │
├───────────────────────────┬─────────────────────────────────┤
│   Bthread (TaskMeta)      │    Coroutine (CoroutineMeta)    │
│   Assembly Context        │    C++20 Compiler Context       │
│   └─ inherits TaskMetaBase ┘   └─ inherits TaskMetaBase     │
├───────────────────────────┴─────────────────────────────────┤
│              Unified Worker Threads (Work Stealing)         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  │ Worker N │   │
│  │          │  │          │  │          │  │          │   │
│  │ Handles: │  │ Handles: │  │ Handles: │  │ Handles: │   │
│  │ - bthread│  │ - bthread│  │ - bthread│  │ - bthread│   │
│  │ - coro   │  │ - coro   │  │ - coro   │  │ - coro   │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
├─────────────────────────────────────────────────────────────┤
│              Platform Abstraction Layer                      │
│              (futex, stack, context switching)              │
└─────────────────────────────────────────────────────────────┘
```

## 核心组件

### 1. TaskMetaBase（统一任务基类）

所有任务类型的基类，提供统一的调度接口。

```cpp
// include/bthread/core/task_meta_base.hpp
namespace bthread {

enum class TaskState : uint8_t {
    READY,      // 就绪，等待执行
    RUNNING,    // 正在执行
    SUSPENDED,  // 挂起（等待锁、条件变量、sleep）
    FINISHED    // 已完成
};

enum class TaskType : uint8_t {
    BTHREAD,    // 汇编上下文切换（有栈协程）
    COROUTINE   // C++20 协程（无栈协程）
};

struct TaskMetaBase {
    // ========== 核心状态 ==========
    std::atomic<TaskState> state{TaskState::READY};
    TaskType type;  // BTHREAD 或 COROUTINE

    // ========== 调度 ==========
    std::atomic<TaskMetaBase*> next{nullptr};  // 队列链接
    Worker* owner_worker{nullptr};             // 所属 Worker

    // ========== 同步 ==========
    void* waiting_sync{nullptr};  // 等待的同步原语

    // ========== 任务标识 ==========
    uint32_t slot_index{0};
    uint32_t generation{0};

    // ========== 虚拟接口 ==========
    virtual ~TaskMetaBase() = default;
    virtual void resume() = 0;  // 恢复执行
};

} // namespace bthread
```

### 2. TaskMeta（bthread 任务）

继承自 TaskMetaBase，添加汇编上下文切换特有的字段。

```cpp
struct TaskMeta : TaskMetaBase {
    TaskMeta() { type = TaskType::BTHREAD; }

    // ========== 栈管理 ==========
    void* stack{nullptr};
    size_t stack_size{0};

    // ========== 上下文 ==========
    platform::Context context{};

    // ========== 入口函数 ==========
    void* (*fn)(void*){nullptr};
    void* arg{nullptr};
    void* result{nullptr};

    // ========== 引用计数 ==========
    std::atomic<int> ref_count{0};

    // ========== Join 支持 ==========
    void* join_butex{nullptr};
    std::atomic<int> join_waiters{0};

    // ========== Butex 等待 ==========
    void* waiting_butex{nullptr};
    WaiterState waiter;

    void resume() override;  // 通过 SwapContext 实现
};
```

### 3. CoroutineMeta（协程任务）

继承自 TaskMetaBase，添加协程特有的字段。

```cpp
struct CoroutineMeta : TaskMetaBase {
    CoroutineMeta() { type = TaskType::COROUTINE; }

    // ========== 协程句柄 ==========
    std::coroutine_handle<> handle;

    // ========== 取消支持 ==========
    std::atomic<bool> cancel_requested{false};

    void resume() override {
        if (handle && !handle.done()) {
            handle.resume();
        }
    }
};
```

### 4. Scheduler（统一调度器）

```cpp
class Scheduler {
public:
    static Scheduler& Instance();

    // ========== 统一任务提交 ==========
    void Submit(TaskMetaBase* task);  // 适用于两种任务类型

    // ========== 协程支持 ==========
    template<typename T>
    coro::Task<T> Spawn(coro::Task<T> task);

    // ========== 生命周期 ==========
    void Init();
    void Shutdown();

    // ========== 访问器 ==========
    GlobalQueue& global_queue();
    TimerThread* GetTimerThread();
    int32_t worker_count() const;

private:
    std::vector<Worker*> workers_;
    GlobalQueue global_queue_;
    std::unique_ptr<TimerThread> timer_thread_;
};
```

### 5. Worker（统一工作线程）

Worker 现在可以执行两种类型的任务。

```cpp
class Worker {
public:
    void Run() {
        while (running_) {
            TaskMetaBase* task = PickTask();
            if (!task) {
                WaitForTask();
                continue;
            }

            current_task_ = task;
            task->state.store(TaskState::RUNNING);
            task->owner_worker = this;

            // 根据任务类型分发执行
            switch (task->type) {
                case TaskType::BTHREAD:
                    RunBthread(static_cast<TaskMeta*>(task));
                    break;
                case TaskType::COROUTINE:
                    RunCoroutine(static_cast<CoroutineMeta*>(task));
                    break;
            }

            HandleTaskAfterRun(current_task_);
            current_task_ = nullptr;
        }
    }

private:
    void RunBthread(TaskMeta* task) {
        SwapContext(&saved_context_, &task->context);
    }

    void RunCoroutine(CoroutineMeta* meta) {
        if (meta->handle && !meta->handle.done()) {
            meta->handle.resume();
        }
    }
};
```

### 6. GlobalQueue（统一全局队列）

```cpp
class GlobalQueue {
public:
    void Push(TaskMetaBase* task);     // 适用于两种类型
    TaskMetaBase* Pop();               // 返回统一基类指针
    bool Empty() const;

private:
    std::atomic<TaskMetaBase*> head_{nullptr};
    std::atomic<TaskMetaBase*> tail_{nullptr};
};
```

## 任务调度流程

### 1. 提交任务

```
Scheduler::Submit(task)
    │
    ├─> task->state = READY
    │
    ├─> if (current_worker)
    │       local_queue.Push(task)
    │   else
    │       global_queue.Push(task)
    │       WakeIdleWorkers()
```

### 2. 选取任务

```
Worker::PickTask()
    │
    ├─> local_queue.Pop()      // 优先本地队列
    │
    ├─> global_queue.Pop()     // 其次全局队列
    │
    └─> StealFromOther()       // 最后工作窃取
```

### 3. 执行任务

```
Worker::Run()
    │
    ├─> task = PickTask()
    │
    ├─> switch (task->type)
    │       case BTHREAD:  RunBthread()
    │       case COROUTINE: RunCoroutine()
    │
    └─> HandleTaskAfterRun(task)
            │
            ├─> FINISHED: 清理资源
            ├─> SUSPENDED: 等待唤醒
            └─> READY: 已重新入队
```

## 统一同步原语

### Mutex（统一互斥锁）

```cpp
class Mutex {
public:
    void lock();           // 阻塞锁（bthread/pthread）
    LockAwaiter lock_async();  // awaitable 锁（协程）
    bool try_lock();
    void unlock();

private:
    std::atomic<uint32_t> state_{0};
    std::atomic<void*> butex_{nullptr};  // bthread 等待
    WaiterNode* waiter_head_{nullptr};    // 协程等待
};
```

### CondVar（统一条件变量）

```cpp
class CondVar {
public:
    void wait(Mutex& mutex);               // 阻塞等待
    WaitAwaiter wait_async(Mutex& mutex);  // awaitable 等待
    void notify_one();
    void notify_all();
};
```

### Event（统一事件）

```cpp
class Event {
public:
    void wait();               // 阻塞等待
    WaitAwaiter wait_async();  // awaitable 等待
    void set();                // 设置事件
    void reset();              // 重置事件
};
```

## 与旧架构的对比

| 特性 | 旧架构 | 新架构 |
|------|--------|--------|
| 调度器 | 分离（Scheduler + CoroutineScheduler） | 统一（Scheduler） |
| 任务基类 | 无 | TaskMetaBase |
| Worker | 仅处理 bthread | 处理两种类型 |
| 全局队列 | 分离（GlobalQueue + CoroutineQueue） | 统一（GlobalQueue） |
| 同步原语 | 分离（Butex + CoMutex） | 统一（Mutex） |

## 性能考虑

1. **虚函数开销**：`resume()` 使用虚函数，但开销可接受
2. **类型检查**：使用 `static_cast` 替代 `dynamic_cast` 避免运行时开销
3. **缓存友好**：TaskMetaBase 大小约 64 字节，适合缓存行

## 文件结构

```
include/bthread/
├── core/
│   ├── task_meta_base.hpp  # 统一任务基类
│   ├── scheduler.hpp       # 统一调度器声明
│   └── task.hpp            # Task 句柄类
├── sync/
│   ├── mutex.hpp           # 统一互斥锁
│   ├── cond.hpp            # 统一条件变量
│   └── event.hpp           # 统一事件
├── scheduler.h             # 调度器实现
├── worker.h                # 工作线程
├── task_meta.h             # TaskMeta 定义
└── global_queue.h          # 统一全局队列

include/coro/
├── meta.h                  # CoroutineMeta 定义
├── coroutine.h             # Task<T>, SafeTask<T>
└── scheduler.h             # 协程包装（委托给 Scheduler）

src/
├── scheduler.cpp           # 统一调度器实现
├── worker.cpp              # Worker 实现
└── ...
```