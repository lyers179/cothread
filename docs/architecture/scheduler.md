# 调度器架构

## 概述

bthread 使用 M:N 调度模型，将 M 个用户级线程（bthread）映射到 N 个内核级线程（worker）上执行。

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        Scheduler                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                   Global Queue                       │   │
│  │  [Task1] -> [Task2] -> [Task3] -> ...               │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  │ Worker N │   │
│  │          │  │          │  │          │  │          │   │
│  │ [Local]  │  │ [Local]  │  │ [Local]  │  │ [Local]  │   │
│  │ [Queue]  │  │ [Queue]  │  │ [Queue]  │  │ [Queue]  │   │
│  │          │  │          │  │          │  │          │   │
│  │ Context  │  │ Context  │  │ Context  │  │ Context  │   │
│  │ Switch   │  │ Switch   │  │ Switch   │  │ Switch   │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
│       │              │              │              │        │
│       └──────────────┴──────────────┴──────────────┘        │
│                         Work Stealing                       │
└─────────────────────────────────────────────────────────────┘
```

## 核心组件

### 1. Scheduler（调度器）

单例模式，管理所有 worker 和全局任务队列。

```cpp
class Scheduler {
    std::vector<Worker*> workers_;      // Worker 列表
    GlobalQueue global_queue_;          // 全局任务队列
    std::atomic<bool> running_;         // 运行标志
    std::unique_ptr<TimerThread> timer_; // 定时器线程

    static Scheduler& Instance();       // 单例访问
    void Init();                        // 初始化
    void Shutdown();                    // 关闭
    void EnqueueTask(TaskMeta* task);   // 任务入队
};
```

### 2. Worker（工作线程）

执行 bthread 的内核线程。

```cpp
class Worker {
    int id_;                            // Worker ID
    WorkStealingQueue local_queue_;     // 本地任务队列
    TaskMeta* current_task_;            // 当前执行的 bthread
    Context saved_context_;             // 保存的上下文
    std::atomic<bool> sleeping_;        // 休眠标志
    std::atomic<int> sleep_token_;      // 休眠令牌

    void Run();                         // 主循环
    TaskMeta* PickTask();               // 选取任务
    void WaitForTask();                 // 等待任务
    void SuspendCurrent();              // 挂起当前 bthread
};
```

### 3. TaskMeta（任务元数据）

bthread 的控制块。

```cpp
struct TaskMeta {
    uint32_t slot_index;                // 槽位索引
    uint32_t generation;                // 代际（用于 ID 编码）
    void* (*fn)(void*);                 // 入口函数
    void* arg;                          // 参数
    void* result;                       // 返回值
    void* stack;                        // 栈指针
    size_t stack_size;                  // 栈大小
    std::atomic<TaskState> state;       // 状态
    std::atomic<int> ref_count;         // 引用计数
    Context context;                    // 上下文
    void* join_butex;                   // join 同步原语
    std::atomic<int> join_waiters;      // join 等待者数量
    WaiterState waiter;                 // 等待状态
};
```

## 任务调度流程

### 1. 创建 bthread

```
bthread_create()
    │
    ├─> AllocTaskMeta()          // 从池中分配
    │
    ├─> AllocateStack()          // 分配栈空间
    │
    ├─> MakeContext()            // 设置初始上下文
    │
    └─> EnqueueTask()            // 加入队列
            │
            ├─> 如果在 Worker 中 -> local_queue.Push()
            │
            └─> 如果在 pthread 中 -> global_queue.Push() + WakeIdleWorkers()
```

### 2. 任务执行

```
Worker::Run() 主循环:
    │
    ├─> PickTask()               // 选取任务
    │       │
    │       ├─> local_queue.Pop()        // 优先本地队列
    │       │
    │       ├─> global_queue.Pop()       // 其次全局队列
    │       │
    │       └─> StealFromOther()         // 最后工作窃取
    │
    ├─> SwapContext()            // 切换到 bthread
    │
    └─> HandleTaskAfterRun()     // 处理任务状态
            │
            ├─> FINISHED -> HandleFinishedTask()
            │
            ├─> SUSPENDED -> 等待 butex
            │
            └─> READY -> 已在队列中
```

### 3. 任务挂起

```
bthread_yield() / butex Wait
    │
    ├─> state = READY/SUSPENDED
    │
    ├─> local_queue.Push()  // 仅 yield
    │
    └─> SwapContext(suspended, saved)  // 切回 Worker
```

### 4. 任务唤醒

```
Butex::Wake()
    │
    ├─> 从等待队列取出任务
    │
    ├─> state = READY
    │
    └─> EnqueueTask()
            │
            └─> 如果 worker 在休眠 -> WakeUp()
```

## 工作窃取

### 原理

当 Worker 的本地队列为空时，从其他 Worker 窃取任务。

```cpp
TaskMeta* Worker::PickTask() {
    // 1. 本地队列
    if (auto* task = local_queue_.Pop()) return task;

    // 2. 全局队列
    if (auto* task = global_queue_.Pop()) return task;

    // 3. 工作窃取
    for (int i = 0; i < attempts; ++i) {
        int victim = random() % worker_count;
        if (victim == id_) continue;

        if (auto* task = workers_[victim]->local_queue().Steal()) {
            return task;
        }
    }

    return nullptr;
}
```

### 双端队列设计

```
本地操作（仅 Owner）:
  Push() ──────────────> [Task N] [Task N-1] ... [Task 1]
                           ↑
                         Top

窃取操作（其他 Worker）:
  [Task N] [Task N-1] ... [Task 1] <────────── Steal()
                                         ↑
                                       Bottom
```

- Push/Pop: 从同一端操作（LIFO），缓存友好
- Steal: 从另一端操作（FIFO），公平性

## 上下文切换

### 汇编实现（x86-64）

```asm
; Windows x64
SwapContext PROC
    ; 保存当前上下文
    mov rax, [rcx]        ; from->stack_ptr
    mov [rax + 0], rbx
    mov [rax + 8], rbp
    mov [rax + 16], rsi
    mov [rax + 24], rdi
    mov [rax + 32], r12
    mov [rax + 40], r13
    mov [rax + 48], r14
    mov [rax + 56], r15
    mov [rax + 64], rsp
    mov [rax + 72], rip

    ; 恢复目标上下文
    mov rax, [rdx]        ; to->stack_ptr
    mov rbx, [rax + 0]
    mov rbp, [rax + 8]
    mov rsi, [rax + 16]
    mov rdi, [rax + 24]
    mov r12, [rax + 32]
    mov r13, [rax + 40]
    mov r14, [rax + 48]
    mov r15, [rax + 56]
    mov rsp, [rax + 64]
    mov rip, [rax +72]

    ret
SwapContext ENDP
```

### 栈布局

```
高地址
┌─────────────────────┐
│   Guard Page        │  ← 栈溢出保护
├─────────────────────┤
│                     │
│   Stack Space       │
│                     │
├─────────────────────┤
│   Initial Frame     │  ← MakeContext 设置
│   Return Addr       │
│   BthreadEntry      │
│   TaskMeta*         │
└─────────────────────┘  ← stack_top
低地址
```

## 性能优化

### 1. 缓存友好

- 本地队列优先：减少跨 CPU 缓存失效
- 栈复用：减少内存分配

### 2. 无锁设计

- 本地队列：单生产者单消费者，无锁
- 全局队列：使用原子操作
- Butex 等待队列：使用 CAS

### 3. 批量操作

- 一次唤醒多个等待者
- 批量任务入队

## 调试技巧

### 启用日志

```cpp
#define BTHREAD_DEBUG 1
#include "bthread/debug.h"

BTHREAD_LOG("Task %p state: %d", task, task->state.load());
```

### 检查死锁

```cpp
// 在 Wait 中添加超时
butex->Wait(generation, &timeout);

if (ret == ETIMEDOUT) {
    BTHREAD_LOG("Possible deadlock detected");
}
```

### 任务追踪

```cpp
void Worker::Run() {
    while (running_) {
        TaskMeta* task = PickTask();
        BTHREAD_LOG("Worker %d picked task %p", id_, task);
        // ...
    }
}
```