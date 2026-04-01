# C++ 协程实现方案详解

## 一、协程概述

协程是一种可以暂停执行并在稍后恢复的函数，它提供了比线程更轻量的并发抽象。协程的核心特点是**协作式调度**——协程主动让出执行权，而非被内核抢占。

---

## 二、有栈协程 vs 无栈协程

### 2.1 有栈协程（Stackful Coroutine）

有栈协程拥有**独立的执行栈**，类似线程但完全在用户态管理。

#### 实现机制

```
┌─────────────────────────────────────────────────────────────┐
│                     主线程栈                                 │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ main()                                               │   │
│  │ ...                                                  │   │
│  │ call_coroutine() ──────────────────────────────────┐│   │
│  └─────────────────────────────────────────────────────┘│  │
│                                                          │  │
│  ┌─────────────────────────────────────────────────────┐│  │
│  │ 协程栈 (独立内存块)                                  ││  │
│  │ ┌─────────────────────────────────────────────────┐││  │
│  │ │ coroutine_fn()                                  │││  │
│  │ │ local_var1                                      │││  │
│  │ │ local_var2                                      │││  │
│  │ │ ...                                             │││  │
│  │ └─────────────────────────────────────────────────┘││  │
│  └─────────────────────────────────────────────────────┘│  │
└─────────────────────────────────────────────────────────────┘
```

#### 核心实现要素

1. **独立栈空间**：每个协程分配独立栈（通常 1MB 或更小）
2. **上下文结构**：保存寄存器状态（callee-saved registers + SP）
3. **栈切换**：汇编实现 `SwapContext`，切换 SP 寄存器

```cpp
// 有栈协程的上下文结构（本项目 bthread 的实现）
struct Context {
    uint64_t gp_regs[16];      // 通用寄存器
    uint8_t xmm_regs[160];     // XMM 寄存器 (Windows)
    void* stack_ptr;           // 栈指针
    void* return_addr;         // 返回地址
};

// 栈切换汇编
SwapContext:
    movq %rbx, 0(%rdi)         // 保存当前寄存器
    movq %rsp, 112(%rdi)       // 保存栈指针
    movq 112(%rsi), %rsp       // 切换到目标栈
    ret                         // 跳转到目标代码
```

#### 特点

| 优点 | 缺点 |
|------|------|
| 可在任意函数调用层级挂起 | 内存占用大（每个协程独立栈） |
| 支持深递归 | 创建开销较大 |
| 实现相对直观 | 栈溢出风险 |
| 不需要编译器特殊支持 | 跨平台汇编实现复杂 |

**典型实现**：Boost.Context、bthread（本项目）、Go goroutine

---

### 2.2 无栈协程（Stackless Coroutine）

无栈协程**没有独立栈**，复用调用者的栈空间，状态保存在编译器生成的**协程帧**中。

#### 实现机制

```
┌─────────────────────────────────────────────────────────────┐
│                     主线程栈                                 │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ main()                                               │   │
│  │ ...                                                  │   │
│  │ caller()                                             │   │
│  │   ┌─────────────────────────────────────────────────┐│  │
│  │   │ 协程帧 (堆分配)                                  ││  │
│  │   │ ┌─────────────────────────────────────────────┐││  │
│  │   │ │ promise_type                                │││  │
│  │   │ │ 挂起点索引 (resume point)                    │││  │
│  │   │ │ 局部变量副本                                │││  │
│  │   │ │ awaiter 状态                                │││  │
│  │   │ └─────────────────────────────────────────────┘││  │
│  │   └─────────────────────────────────────────────────┘│  │
│  │   co_await 挂起 ──> 返回到 caller                    │   │
│  │   resume() ──> 从帧恢复状态继续执行                   │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

#### 核心实现要素

1. **协程帧（Coroutine Frame）**：编译器生成的状态结构
2. **挂起点索引**：记录执行到哪个 `co_await`，恢复时跳转
3. **状态保存**：局部变量、临时对象保存在帧中
4. **promise_type**：定制协程行为的接口

#### 特点

| 优点 | 缺点 |
|------|------|
| 内存占用极小（仅帧大小） | 只能在 `co_await` 点挂起 |
| 创建开销低（堆分配帧） | 编译器必须支持 C++20 |
| 无栈溢出风险 | 跨调用层级挂起受限 |
| 可内联优化 | 理解成本高 |

**典型实现**：C++20 Coroutines、Rust async/await、C# async/await

---

### 2.3 对比总结

```
                    有栈协程                    无栈协程
                    ─────────                   ─────────
栈空间              独立栈 (1MB)                复用调用栈
内存开销            O(协程数 × 栈大小)          O(协程数 × 帧大小)
挂起点              任意位置                    co_await 点
创建速度            较慢 (分配栈)               快 (分配帧)
上下文切换          汇编切换栈                  函数返回 + resume
编译器支持          不需要                      C++20 必需
典型应用            bthread, goroutine          C++20, async/await
```

---

## 三、C++20 协程用法

### 3.1 基础概念

C++20 协程使用三个关键字：
- `co_await`：挂起协程，等待异步操作完成
- `co_return`：协程返回值
- `co_yield`：生成值并挂起（用于生成器）

### 3.2 最简协程示例

```cpp
#include <coroutine>
#include <iostream>

// 协程返回类型
struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;
};

// 协程函数
Task simple_coroutine() {
    std::cout << "Hello from coroutine!\n";
    co_return;  // 协程结束
}

int main() {
    auto task = simple_coroutine();  // 创建协程（挂起在初始点）
    task.handle.resume();            // 恢复执行
    task.handle.destroy();           // 销毁协程帧
}
```

### 3.3 本项目 Task<T> 实现

[coroutine.h](include/coro/coroutine.h) 展示了完整的 Task 实现：

```cpp
template<typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;  // 必须定义 promise_type

    // Awaiter 接口：支持 co_await Task<T>
    bool await_ready() { return handle_ && handle_.done(); }
    void await_suspend(std::coroutine_handle<> awaiting) {
        handle_.promise().set_awaiter(awaiting);
    }
    T await_resume() { return get(); }

private:
    std::coroutine_handle<promise_type> handle_;
};

template<typename T>
class TaskPromise {
public:
    Task<T> get_return_object() {
        return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
    }

    // 初始挂起：协程创建后不立即执行
    std::suspend_always initial_suspend() noexcept { return {}; }

    // 最终挂起：协程完成后不自动销毁，等待获取结果
    auto final_suspend() noexcept {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<TaskPromise<T>> h) noexcept {
                // 返回等待者，唤醒调用者
                return h.promise().awaiter_;
            }
            void await_resume() noexcept {}
        };
        return FinalAwaiter{};
    }

    void return_value(T value) { result_ = std::move(value); }
    T get_result() { return std::move(result_); }

private:
    T result_;
    std::coroutine_handle<> awaiter_;  // 等待此协程的调用者
};
```

### 3.4 使用示例（本项目 demo）

[coro_demo.cpp](demo/coro_demo.cpp)：

```cpp
// 定义协程
coro::Task<void> demo_task(int id, coro::CoMutex& m, std::atomic<int>& counter) {
    std::cerr << "Task " << id << " starting\n";

    co_await m.lock();           // 协程式加锁
    counter++;
    std::cerr << "Task " << id << " incremented counter to " << counter << "\n";
    m.unlock();

    co_await coro::yield();      // 主动让出

    std::cerr << "Task " << id << " done\n";
}

int main() {
    coro::CoroutineScheduler::Instance().Init();

    coro::CoMutex mutex;
    std::atomic<int> counter{0};

    // 启动多个协程
    for (int i = 0; i < 5; ++i) {
        coro::co_spawn(demo_task(i, mutex, counter));
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    coro::CoroutineScheduler::Instance().Shutdown();
}
```

### 3.5 yield 和 sleep

```cpp
// yield() - 让出执行权
coro::Task<void> cooperative_work() {
    for (int i = 0; i < 10; ++i) {
        std::cout << i << std::endl;
        co_await coro::yield();  // 其他协程可以执行
    }
}

// sleep() - 定时挂起
coro::Task<void> delayed_work() {
    std::cout << "Start" << std::endl;
    co_await coro::sleep(std::chrono::milliseconds(1000));
    std::cout << "After 1 second" << std::endl;
}
```

### 3.6 协程同步原语

```cpp
// CoMutex - 协程互斥锁
coro::Task<void> protected_work(coro::CoMutex& m) {
    co_await m.lock();    // 挂起而非阻塞线程
    // ... 临界区操作 ...
    m.unlock();
}

// CoCond - 协程条件变量
coro::Task<void> consumer(coro::CoMutex& m, coro::CoCond& c, int& data) {
    co_await m.lock();
    while (data == 0) {
        co_await c.wait(m);  // 挂起等待信号
    }
    m.unlock();
}
```

---

## 四、C++20 协程实现原理

### 4.1 编译器转换

编译器将协程函数转换为**状态机**，每个 `co_await` 对应一个状态。

**原始协程代码**：
```cpp
Task<int> example() {
    int a = 1;
    co_await yield();
    int b = a + 2;
    co_await yield();
    co_return b;
}
```

**编译器生成的伪代码**：
```cpp
struct ExampleFrame {
    // 状态机索引
    int state = 0;

    // promise_type
    TaskPromise<int> promise;

    // 局部变量（跨挂起点存活）
    int a;
    int b;

    // 参数副本
    // (无参数)

    // awaiter 临时对象
    YieldAwaiter awaiter1;
    YieldAwaiter awaiter2;
};

void example_resume(ExampleFrame* frame) {
    switch (frame->state) {
        case 0:  // 初始状态
            frame->a = 1;
            frame->awaiter1 = yield();
            if (!frame->awaiter1.await_ready()) {
                frame->state = 1;
                frame->awaiter1.await_suspend(frame->promise.handle);
                return;  // 挂起
            }
            goto state1;

        case 1:  // 第一个 co_await 后
            frame->awaiter1.await_resume();
            frame->b = frame->a + 2;
            frame->awaiter2 = yield();
            if (!frame->awaiter2.await_ready()) {
                frame->state = 2;
                frame->awaiter2.await_suspend(frame->promise.handle);
                return;  // 挂起
            }
            goto state2;

        case 2:  // 第二个 co_await 后
            frame->awaiter2.await_resume();
            frame->promise.return_value(frame->b);
            frame->promise.final_suspend();
            return;  // 完成
    }
}
```

### 4.2 协程帧布局

```
┌─────────────────────────────────────────────────────────────┐
│                     Coroutine Frame                          │
├─────────────────────────────────────────────────────────────┤
│  promise_type                                                │
│    ├─ get_return_object() 返回的 Task                        │
│    ├─ result_ (返回值存储)                                   │
│    ├─ awaiter_ (等待者句柄)                                  │
│    └─ exception_ (异常存储)                                  │
├─────────────────────────────────────────────────────────────┤
│  State Index (状态机索引)                                    │
│    ├─ 0: 初始                                                │
│    ├─ 1: 第一个 co_await 后                                  │
│    ├─ 2: 第二个 co_await 后                                  │
│    └─ -1: 完成                                               │
├─────────────────────────────────────────────────────────────┤
│  Local Variables (跨挂起点的局部变量)                         │
│    ├─ int a                                                  │
│    ├─ int b                                                  │
│    └─ ...                                                    │
├─────────────────────────────────────────────────────────────┤
│  Parameters (参数副本)                                       │
│    ├─ 参数1                                                  │
│    ├─ 参数2                                                  │
│    └─ ...                                                    │
├─────────────────────────────────────────────────────────────┤
│  Awaiter Temporaries (每个 co_await 的 awaiter)             │
│    ├─ awaiter1 (第一个 yield)                                │
│    ├─ awaiter2 (第二个 yield)                                │
│    └─ ...                                                    │
└─────────────────────────────────────────────────────────────┘
```

### 4.3 三大核心类型

#### promise_type

定制协程行为，必须定义在返回类型中：

```cpp
struct promise_type {
    // 创建返回对象
    Task get_return_object();

    // 初始挂起：创建后是否立即执行
    auto initial_suspend();

    // 最终挂起：完成后行为
    auto final_suspend() noexcept;

    // 处理返回值
    void return_value(T);
    void return_void();

    // 处理异常
    void unhandled_exception();

    // 自定义内存分配（可选）
    void* operator new(size_t);
    void operator delete(void*);
};
```

#### std::coroutine_handle

协程句柄，用于手动控制协程：

```cpp
template<typename Promise>
class coroutine_handle {
public:
    // 从 promise 创建
    static coroutine_handle from_promise(Promise&);

    // 恢复执行
    void resume();

    // 销毁帧
    void destroy();

    // 检查是否完成
    bool done() const;

    // 获取 promise
    Promise& promise() const;
};
```

#### Awaiter

定义 `co_await` 行为：

```cpp
struct Awaiter {
    // 是否需要挂起
    bool await_ready();

    // 挂起时执行
    // 返回 void: 挂起
    // 返回 bool: true=挂起, false=立即恢复
    // 返回 handle: 切换到另一个协程
    void/bool/coroutine_handle<> await_suspend(coroutine_handle<>);

    // 恢复时执行，返回值为 co_await 表达式的结果
    T await_resume();
};
```

### 4.4 co_await 执行流程

```
co_await expr
    │
    ├─ 1. 获取 awaiter
    │      auto&& awaiter = get_awaiter(expr);
    │
    ├─ 2. 检查是否需要挂起
    │      if (!awaiter.await_ready()) {
    │          // 需要挂起
    │      }
    │
    ├─ 3. 挂起前保存状态
    │      编译器保存当前状态索引、局部变量到帧
    │
    ├─ 4. 执行 await_suspend
    │      auto result = awaiter.await_suspend(handle);
    │      │
    │      ├─ return void: 挂起，返回调用者
    │      ├─ return false: 立即恢复
    │      └─ return handle: 切换到指定协程
    │
    ├─ 5. （恢复后）执行 await_resume
    │      auto result = awaiter.await_resume();
    │      // result 成为 co_await 表达式的值
    │
    └─ 6. 继续执行协程后续代码
```

### 4.5 本项目 YieldAwaiter 实现

[coroutine.cpp](src/coro/coroutine.cpp)：

```cpp
class YieldAwaiter {
public:
    bool await_ready() noexcept { return false; }  // 总是挂起

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        CoroutineMeta* meta = current_coro_meta();
        if (meta) {
            // 设置 READY 状态，重新入队
            meta->state.store(CoroutineMeta::READY, std::memory_order_release);
            CoroutineScheduler::Instance().EnqueueCoroutine(meta);
            return true;  // 确实挂起
        }
        return false;  // 非调度器上下文，立即恢复
    }

    void await_resume() noexcept {}  // 无返回值
};
```

### 4.6 FinalAwaiter 的关键作用

协程完成时，需要唤醒等待者：

```cpp
struct FinalAwaiter {
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<TaskPromise<T>> h) noexcept {
        // 返回等待者的句柄，立即切换到它
        return h.promise().awaiter_;
        // 如果没有等待者，返回 noop_coroutine()
    }

    void await_resume() noexcept {}
};
```

这实现了**对称切换**：协程完成时直接跳转到等待者，避免额外的调度开销。

---

## 五、协程帧内存管理

### 5.1 默认堆分配

默认情况下，协程帧通过 `operator new` 在堆分配：

```cpp
void* TaskPromise::operator new(size_t size) {
    return ::operator new(size);  // 堆分配
}
```

### 5.2 本项目 FramePool 优化

[frame_pool.h](include/coro/frame_pool.h) 实现池化分配：

```cpp
class FramePool {
public:
    void Init(size_t block_size, size_t initial_count);
    void* Allocate(size_t size);
    void Deallocate(void* block);

private:
    std::vector<void*> blocks_;
    std::atomic<void*> free_list_;
};

// promise_type 使用池
void* TaskPromise::operator new(size_t size) {
    return FramePool::Instance().Allocate(size);
}

void TaskPromise::operator delete(void* ptr) {
    FramePool::Instance().Deallocate(ptr);
}
```

**池化优势**：
- 预分配固定大小块（如 8KB）
- 避免 repeated malloc/free
- 缓存友好（连续内存）

---

## 六、协程调度

### 6.1 调度器架构

本项目协程池使用专用 Worker 线程：

```
┌─────────────────────────────────────────────────────────────┐
│                   CoroutineScheduler                         │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                 GlobalQueue                          │   │
│  │  [Coroutine1] -> [Coroutine2] -> ...                │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  │ Worker 3 │   │
│  │          │  │          │  │          │  │          │   │
│  │  Loop:   │  │  Loop:   │  │  Loop:   │  │  Loop:   │   │
│  │  Pop()   │  │  Pop()   │  │  Pop()   │  │  Pop()   │   │
│  │  Resume()│  │  Resume()│  │  Resume()│  │  Resume()│   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 Worker 主循环

```cpp
void CoroutineWorkerLoop() {
    while (running_) {
        CoroutineMeta* meta = global_queue_.Pop();
        if (!meta) {
            WaitForCoroutine();
            continue;
        }

        current_coro_meta_ = meta;
        meta->state = CoroutineMeta::RUNNING;
        meta->handle.resume();          // 恢复协程
        current_coro_meta_ = nullptr;

        // 处理挂起/完成状态
        if (meta->state == CoroutineMeta::FINISHED) {
            FreeMeta(meta);
        }
    }
}
```

### 6.3 协程状态流转

```
         co_spawn()
             │
             ▼
        ┌─────────┐
        │  READY  │ ──────┐
        └─────────┘       │
             │            │
        Worker.pop()      │
             │            │
             ▼            │
        ┌─────────┐       │
        │ RUNNING │       │
        └─────────┘       │
             │            │
    ┌────────┴────────┐   │
    ▼                 ▼   │
┌──────────┐    ┌─────────┴───┐
│SUSPENDED │    │  FINISHED   │
│(co_await)│    │ (co_return) │
└──────────┘    └─────────────┘
    │                 │
    │ 唤醒            │ FreeMeta()
    │                 │
    └─────────────────┘
         回到 READY
```

---

## 七、与 bthread 的对比

| 特性 | bthread (有栈) | C++20 协程 (无栈) |
|------|----------------|-------------------|
| 栈管理 | 独立栈 (1MB) | 帧池 (8KB) |
| 挂起点 | 任意位置 | co_await 点 |
| 上下文切换 | 汇编 SwapContext | 函数返回 + resume |
| API 风格 | `bthread_create()` | `co_spawn()` |
| 同步原语 | Butex | CoMutex/CoCond |
| 内存开销 | ~1MB/协程 | ~8KB/协程 |
| 跨平台 | 需汇编适配 | 编译器统一处理 |

---

## 八、总结

### 有栈协程适合场景
- 需要在任意位置挂起（如第三方库中的阻塞调用）
- 已有大量同步代码需要异步化
- 平台特定优化（如 Linux futex 集成）

### 无栈协程适合场景
- 新代码，从零设计异步流程
- 内存敏感（大量并发任务）
- 现代异步 I/O 集成（socket、文件）

### 本项目选择
本项目同时实现了两种方案：
- **bthread**：有栈协程，用于高性能 M:N 线程池
- **coro**：C++20 无栈协程，提供现代 async/await API

两者可独立使用，未来版本可能统一调度器。