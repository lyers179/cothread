# C++20 协程编译器转换详解

## 一、协程是语法糖

C++20 协程确实是**语法糖**——编译器将协程函数转换为：
1. **协程帧（Coroutine Frame）**：存储状态的堆对象
2. **状态机代码**：根据恢复点跳转到对应代码块
3. **promise_type 交互**：通过约定接口定制行为

---

## 二、无栈协程的核心原理

### 2.1 为什么叫"无栈"？

无栈协程**没有独立的执行栈**，而是复用调用者的栈空间。状态保存在堆上的**协程帧**中，而非栈上。

### 2.2 有栈协程 vs 无栈协程的栈模型

#### 有栈协程：栈切换

```
                    有栈协程执行模型
═══════════════════════════════════════════════════════════════

时间 T1: Worker 线程执行，调用协程 A
┌─────────────────────────────────────────────────────────────┐
│                  Worker 线程栈                              │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ main()                                                 │ │
│  │   worker_loop()                                        │ │
│  │     pick_task()                                        │ │
│  │     SwapContext() ─────────────────────────────────┐  │ │
│  └─────────────────────────────────────────────────────┘  │ │
│                                                            │ │
│  ┌─────────────────────────────────────────────────────┐  │ │
│  │ 协程 A 的独立栈 (1MB)                                │  │ │
│  │ ┌─────────────────────────────────────────────────┐ │  │ │
│  │ │ coroutine_a()                                   │ │  │ │
│  │ │   local_var1 = 10                               │ │  │ │
│  │ │   local_var2 = 20                               │ │  │ │
│  │ │   ...执行中...                                  │ │  │ │
│  │ └─────────────────────────────────────────────────┘ │  │ │
│  │                    ↑ rsp 指向这里                    │  │ │
│  └─────────────────────────────────────────────────────┘  │ │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ SwapContext() 切换 rsp
                          ▼

时间 T2: 协程 A 挂起，切换到协程 B
┌─────────────────────────────────────────────────────────────┐
│                  Worker 线程栈                              │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ main()                                                 │ │
│  │   worker_loop()                                        │ │
│  │     pick_task()                                        │ │
│  │     SwapContext()                                      │ │
│  └───────────────────────────────────────────────────────┘ │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ 协程 B 的独立栈 (1MB)                                │   │
│  │ ┌─────────────────────────────────────────────────┐ │   │
│  │ │ coroutine_b()                                   │ │   │
│  │ │   local_var3 = 30                               │ │   │
│  │ │   ...执行中...                                  │ │   │
│  │ └─────────────────────────────────────────────────┘ │   │
│  │                    ↑ rsp 切换到这里                  │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ 协程 A 的独立栈 (挂起状态)                           │   │
│  │ ┌─────────────────────────────────────────────────┐ │   │
│  │ │ coroutine_a()                                   │ │   │
│  │ │   local_var1 = 10  ← 保存在栈上                 │ │   │
│  │ │   local_var2 = 20  ← 保存在栈上                 │ │   │
│  │ └─────────────────────────────────────────────────┘ │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘

关键点：
- 每个协程有独立栈内存（通常 1MB）
- 切换协程 = 切换 rsp 寄存器（汇编 SwapContext）
- 局部变量保存在各自的栈上
- 内存占用：O(协程数 × 栈大小)
```

#### 无栈协程：帧在堆上，复用调用栈

```
                    无栈协程执行模型
═══════════════════════════════════════════════════════════════

时间 T1: Worker 线程调用 resume()，执行协程 A
┌─────────────────────────────────────────────────────────────┐
│                  Worker 线程栈（唯一栈）                     │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ main()                                                 │ │
│  │   worker_loop()                                        │ │
│  │     resume() ───────────────────────────────────────┐ │ │
│  │       │                                              │ │ │
│  │       │   ┌────────────────────────────────────────┐│ │ │
│  │       │   │ 协程 A 的代码（直接在调用栈上执行）      ││ │ │
│  │       │   │                                         ││ │ │
│  │       │   │   int a = frame_A->__local_a;  ← 从帧读取│ │ │
│  │       │   │   int b = a + 1;                       ││ │ │
│  │       │   │   // 执行到 co_await                    ││ │ │
│  │       │   │   return;  ← 挂起，返回到 worker_loop   ││ │ │
│  │       │   └────────────────────────────────────────┘│ │ │
│  │       │                                              │ │ │
│  │       └──────────────────────────────────────────────┘ │ │
│  └───────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ 帧在堆上，不在栈上
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                        堆内存                               │
│  ┌─────────────────┐  ┌─────────────────┐                  │
│  │ 协程帧 A         │  │ 协程帧 B         │                  │
│  │ ┌─────────────┐ │  │ ┌─────────────┐ │                  │
│  │ │ state = 2   │ │  │ │ state = 0   │ │                  │
│  │ │ __local_a   │ │  │ │ __local_x   │ │                  │
│  │ │ __local_b   │ │  │ │ __awaiter_0 │ │                  │
│  │ │ __awaiter_0 │ │  │ │ promise     │ │                  │
│  │ │ promise     │ │  │ └─────────────┘ │                  │
│  │ └─────────────┘ │  │                 │                  │
│  └─────────────────┘  └─────────────────┘                  │
└─────────────────────────────────────────────────────────────┘

时间 T2: 协程 A 挂起，Worker 调用 resume() 执行协程 B
┌─────────────────────────────────────────────────────────────┐
│                  Worker 线程栈（同一个栈）                   │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ main()                                                 │ │
│  │   worker_loop()                                        │ │
│  │     resume() ───────────────────────────────────────┐ │ │
│  │       │                                              │ │ │
│  │       │   ┌────────────────────────────────────────┐│ │ │
│  │       │   │ 协程 B 的代码（复用同一个栈执行）        ││ │ │
│  │       │   │                                         ││ │ │
│  │       │   │   int x = frame_B->__local_x;  ← 从帧读取│ │ │
│  │       │   │   ...                                   ││ │ │
│  │       │   └────────────────────────────────────────┘│ │ │
│  │       │                                              │ │ │
│  │       └──────────────────────────────────────────────┘ │ │
│  └───────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘

关键点：
- 只有一个栈（Worker 线程栈）
- 协程帧在堆上（通常 1-8KB）
- 切换协程 = 函数返回 + 函数调用（无汇编）
- 局部变量保存在帧中，跨挂起点存活
- 内存占用：O(协程数 × 帧大小)
```

### 2.3 无栈协程的"转移"机制

无栈协程的"转移"实际上是**函数调用/返回**，而非栈切换：

```
                    无栈协程控制流转移
═══════════════════════════════════════════════════════════════

步骤 1: 初始调用
───────────────────────────────────────────────────────────────

    Worker 线程                           协程帧 A
    ┌─────────────┐                      ┌─────────────┐
    │ worker_loop │                      │ state = 0   │
    │     │       │                      │ local_a = ? │
    │     ▼       │                      │ awaiter_0   │
    │   resume()  │ ────── 调用 ────────>│             │
    │     │       │                      │             │
    │     │       │                      │             │
    └─────────────┘                      └─────────────┘
          │
          │ 执行协程 A 的代码（在 Worker 栈上）
          │
          ▼

步骤 2: 执行到 co_await，挂起
───────────────────────────────────────────────────────────────

    Worker 线程                           协程帧 A
    ┌─────────────┐                      ┌─────────────┐
    │ worker_loop │                      │ state = 2   │ ← 更新状态
    │     │       │                      │ local_a = 10│ ← 保存局部变量
    │     │       │                      │ awaiter_0   │
    │     │       │ <───── 返回 ─────────│             │
    │     ▼       │                      │             │
    │  (继续循环) │                      │             │
    └─────────────┘                      └─────────────┘
          │
          │ return; 挂起协程 A
          │ 控制权返回 Worker
          │
          ▼

步骤 3: 切换到协程 B
───────────────────────────────────────────────────────────────

    Worker 线程                           协程帧 B
    ┌─────────────┐                      ┌─────────────┐
    │ worker_loop │                      │ state = 0   │
    │     │       │                      │ local_x = ? │
    │     ▼       │                      │ awaiter_0   │
    │   resume()  │ ────── 调用 ────────>│             │
    │     │       │                      │             │
    │     │       │                      │             │
    └─────────────┘                      └─────────────┘
          │
          │ 执行协程 B 的代码（复用同一个栈）
          │
          ▼
```

### 2.4 对称切换（Symmetric Transfer）

协程之间可以直接切换，无需返回调度器：

```
                    对称切换：协程 A → 协程 B
═══════════════════════════════════════════════════════════════

协程 A 执行 co_await task_b（task_b 是协程 B 的 Task）

时间 T1: 协程 A 遇到 co_await task_b
┌─────────────────────────────────────────────────────────────┐
│                  Worker 线程栈                              │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ worker_loop()                                          │ │
│  │   resume(handle_A)                                     │ │
│  │     ┌─────────────────────────────────────────────────┐│ │
│  │     │ 协程 A 的代码                                    ││ │
│  │     │   int result = co_await task_b;                 ││ │
│  │     │                     │                           ││ │
│  │     │                     │ 检查 task_b 是否完成      ││ │
│  │     │                     │ 未完成 → 记录等待者       ││ │
│  │     │                     │                           ││ │
│  │     │   await_suspend() 返回 handle_B                 ││ │
│  │     │                     │                           ││ │
│  │     │   jmp handle_B ─────┼───────────────────────────┼─┐
│  │     └─────────────────────┼───────────────────────────┘ │
│  └───────────────────────────┼─────────────────────────────┘ │
└──────────────────────────────┼───────────────────────────────┘
                               │
                               │ 直接跳转（无 return）
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                  Worker 线程栈（同一个栈帧）                 │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ worker_loop()                                          │ │
│  │   resume(handle_A)                                     │ │
│  │     ┌─────────────────────────────────────────────────┐│ │
│  │     │ 协程 B 的代码（直接开始执行）                    ││ │
│  │     │   int x = 10;                                   ││ │
│  │     │   co_return x * 2;                              ││ │
│  │     │                     │                           ││ │
│  │     │   final_suspend() 返回 handle_A                 ││ │
│  │     │                     │                           ││ │
│  │     │   jmp handle_A ────┼────────────────────────────┼─┐
│  │     └─────────────────────┼───────────────────────────┘ │
│  └───────────────────────────┼─────────────────────────────┘ │
└──────────────────────────────┼───────────────────────────────┘
                               │
                               │ 直接跳转回协程 A
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                  Worker 线程栈                               │
│  ┌───────────────────────────────────────────────────────┐ │
│  │ worker_loop()                                          │ │
│  │   resume(handle_A)  ← 还是同一个调用                   │ │
│  │     ┌─────────────────────────────────────────────────┐│ │
│  │     │ 协程 A 继续（恢复点）                            ││ │
│  │     │   int result = co_await task_b;                 ││ │
│  │     │   // result = 20                                ││ │
│  │     │   co_return result + 1;                         ││ │
│  │     └─────────────────────────────────────────────────┘│ │
│  └───────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘

关键点：
- 无 return 到 worker_loop
- 栈深度不变（无递归增长）
- await_suspend 返回 coroutine_handle 实现对称切换
```

### 2.5 栈深度对比

```
有栈协程：
═══════════════════════════════════════════════════════════════

Worker 调用 resume()
    │
    └──> SwapContext() 切换 rsp
          │
          └──> 协程代码执行（在不同的栈上）
                │
                └──> 协程调用普通函数
                      │
                      └──> 栈在协程的独立栈上增长

栈深度：每个协程独立计算，互不影响


无栈协程：
═══════════════════════════════════════════════════════════════

Worker 调用 resume()
    │
    └──> 协程代码执行（在 Worker 栈上）
          │
          └──> 协程调用普通函数
                │
                └──> 栈在 Worker 栈上增长
                      │
                      └──> co_await 另一个协程
                            │
                            └──> 对称切换（return + call）
                                  │
                                  └──> 另一个协程执行
                                        │
                                        └──> 栈继续增长

栈深度：所有协程共享同一个栈，深度累加！

风险：如果协程 A co_await 协程 B，B co_await 协程 C...
      可能导致栈溢出！

解决方案：
- 限制协程嵌套深度
- 使用调度器中转（非对称切换）
- 尾调用优化（对称切换）
```

### 2.6 对比总结表

| 特性 | 有栈协程 | 无栈协程 |
|------|---------|---------|
| **栈空间** | 每协程独立栈（~1MB） | 共享调用栈，帧在堆（~8KB） |
| **切换机制** | 汇编 SwapContext | 函数 return + call |
| **切换开销** | 保存/恢复寄存器（~100ns） | 函数调用开销（~10ns） |
| **挂起点** | 任意位置 | 仅 co_await 点 |
| **栈溢出风险** | 单协程栈溢出 | 嵌套协程导致栈溢出 |
| **内存占用** | 高（协程数 × 1MB） | 低（协程数 × 8KB） |
| **跨平台** | 需汇编适配 | 编译器统一处理 |

---

## 三、协程帧的创建

### 2.1 帧的结构

编译器为每个协程生成一个**协程帧类型**，包含：

```cpp
// 编译器生成的帧结构（伪代码）
struct __coroutine_frame_example {
    // ========== 编译器内置字段 ==========
    void (*__resume_fn)(__coroutine_frame_example*);  // resume 函数指针
    void (*__destroy_fn)(__coroutine_frame_example*); // destroy 函数指针
    uint16_t __state_index;                            // 状态机索引

    // ========== promise_type ==========
    TaskPromise<int> __promise;                        // promise 对象

    // ========== 参数副本 ==========
    int __param_x;                                     // 函数参数副本
    std::string __param_y;                             // 需要拷贝/移动的参数

    // ========== 局部变量（跨挂起点存活） ==========
    int __local_a;                                     // 局部变量
    std::vector<int> __local_vec;                      // 需要析构的对象

    // ========== Awaiter 临时对象 ==========
    YieldAwaiter __awaiter_0;                          // 第一个 co_await 的 awaiter
    SleepAwaiter __awaiter_1;                          // 第二个 co_await 的 awaiter
};
```

### 2.2 帧的分配

协程调用时，编译器插入帧分配代码：

```cpp
// 原始协程
Task<int> example(int x) {
    int a = x + 1;
    co_await yield();
    co_return a;
}

// 编译器转换后的入口（伪代码）
Task<int> example(int x) {
    // 1. 计算帧大小
    size_t frame_size = sizeof(__coroutine_frame_example);

    // 2. 分配帧（调用 promise_type::operator new）
    void* frame_mem = TaskPromise<int>::operator new(frame_size);

    // 3. 构造帧
    __coroutine_frame_example* frame = new (frame_mem) __coroutine_frame_example;

    // 4. 初始化函数指针
    frame->__resume_fn = &__example_resume;
    frame->__destroy_fn = &__example_destroy;

    // 5. 初始化状态索引
    frame->__state_index = 0;  // 初始状态

    // 6. 构造 promise
    new (&frame->__promise) TaskPromise<int>();

    // 7. 保存参数
    frame->__param_x = x;

    // 8. 调用 get_return_object
    Task<int> result = frame->__promise.get_return_object();

    // 9. 调用 initial_suspend
    auto&& awaiter = frame->__promise.initial_suspend();
    if (!awaiter.await_ready()) {
        // 挂起：保存帧句柄到 Task
        // 返回 Task 给调用者
        return result;
    }

    // 如果 initial_suspend 返回不挂起，立即开始执行
    __example_resume(frame);
    return result;
}
```

---

## 四、状态机转换

### 4.1 状态索引

每个 `co_await`、`co_yield`、`co_return` 都是一个**恢复点（resume point）**，编译器为其分配索引：

```cpp
// 原始协程
Task<int> compute(int x) {
    int a = x + 1;           // ─┐
                             //  │ 状态 0：初始执行
    co_await yield();        // ─┘ 挂起点 1

    int b = a * 2;           // ─┐
                             //  │ 状态 2：第一个 co_await 后
    co_await sleep(100ms);   // ─┘ 挂起点 3

    co_return b;             // ── 状态 4：完成
}

// 状态索引分配：
// 0: 初始执行
// 1: 挂起在 yield()
// 2: yield() 后恢复
// 3: 挂起在 sleep()
// 4: sleep() 后恢复，即将返回
```

### 4.2 状态机代码

编译器将协程体转换为 `switch-case` 状态机：

```cpp
// 编译器生成的 resume 函数（伪代码）
void __compute_resume(__coroutine_frame_compute* frame) {
    // 获取 promise 引用
    TaskPromise<int>& promise = frame->__promise;

    try {
        switch (frame->__state_index) {

        // ========== 状态 0：初始执行 ==========
        case 0:
            goto __state_0;

        __state_0:
            // int a = x + 1;
            frame->__local_a = frame->__param_x + 1;

            // co_await yield();
            {
                // 构造 awaiter
                frame->__awaiter_0 = yield();

                // 检查是否需要挂起
                if (!frame->__awaiter_0.await_ready()) {
                    // 设置下一个状态
                    frame->__state_index = 2;
                    // 挂起
                    frame->__awaiter_0.await_suspend(
                        std::coroutine_handle<>::from_address(frame)
                    );
                    return;  // 返回到调用者
                }
            }
            // 不挂起，继续执行
            goto __state_2;

        // ========== 状态 2：yield() 后恢复 ==========
        case 2:
            goto __state_2;

        __state_2:
            // await_resume
            frame->__awaiter_0.await_resume();

            // int b = a * 2;
            frame->__local_b = frame->__local_a * 2;

            // co_await sleep(100ms);
            {
                frame->__awaiter_1 = sleep(std::chrono::milliseconds(100));

                if (!frame->__awaiter_1.await_ready()) {
                    frame->__state_index = 4;
                    frame->__awaiter_1.await_suspend(
                        std::coroutine_handle<>::from_address(frame)
                    );
                    return;
                }
            }
            goto __state_4;

        // ========== 状态 4：sleep() 后恢复 ==========
        case 4:
            goto __state_4;

        __state_4:
            frame->__awaiter_1.await_resume();

            // co_return b;
            promise.return_value(frame->__local_b);
            goto __final_suspend;

        // ========== 最终挂起 ==========
        __final_suspend:
            {
                auto&& final_awaiter = promise.final_suspend();
                if (!final_awaiter.await_ready()) {
                    frame->__state_index = UINT16_MAX;  // 标记完成
                    final_awaiter.await_suspend(
                        std::coroutine_handle<>::from_address(frame)
                    );
                    return;
                }
            }
            break;
        }

    } catch (...) {
        // 异常处理
        promise.unhandled_exception();
        goto __final_suspend;
    }
}
```

### 4.3 状态转换图

```
                    co_await yield()
    ┌─────────┐     (检查 await_ready)      ┌─────────┐
    │ State 0 │ ───────────────────────────>│ State 1 │
    │ 初始    │                              │ 挂起中  │
    └─────────┘                              └─────────┘
         │                                        │
         │ await_ready == true                    │ resume()
         │ (不挂起)                               │
         ▼                                        ▼
    ┌─────────┐     co_await sleep()       ┌─────────┐
    │ State 2 │ ───────────────────────────>│ State 3 │
    │ yield后 │     (检查 await_ready)      │ 挂起中  │
    └─────────┘                              └─────────┘
         │                                        │
         │ await_ready == true                    │ resume()
         │                                        │
         ▼                                        ▼
    ┌─────────┐     co_return b           ┌─────────┐
    │ State 4 │ ───────────────────────────>│ State   │
    │ sleep后 │                              │ FINISHED│
    └─────────┘                              └─────────┘
```

---

## 五、co_await 的完整转换

### 5.1 三步协议

`co_await expr` 被转换为三个步骤：

```cpp
// 原始代码
auto result = co_await expr;

// 编译器转换
{
    // 步骤 1: 获取 awaiter
    auto&& __awaiter = get_awaiter(expr);

    // 步骤 2: 检查是否需要挂起
    if (!__awaiter.await_ready()) {
        // 保存当前状态
        frame->__state_index = NEXT_STATE;

        // 挂起
        // await_suspend 的返回值决定行为：
        // - void: 挂起，返回调用者
        // - bool: true=挂起, false=立即恢复
        // - coroutine_handle: 切换到指定协程
        using ResultType = decltype(__awaiter.await_suspend(handle));
        if constexpr (std::is_same_v<ResultType, void>) {
            __awaiter.await_suspend(handle);
            return;  // 挂起
        } else if constexpr (std::is_same_v<ResultType, bool>) {
            if (__awaiter.await_suspend(handle)) {
                return;  // 挂起
            }
            // false: 立即恢复，继续执行
        } else {
            // coroutine_handle: 对称切换
            auto next_handle = __awaiter.await_suspend(handle);
            next_handle.resume();  // 切换到另一个协程
            return;
        }
    }

    // 步骤 3: 恢复时执行 await_resume
    auto result = __awaiter.await_resume();
}
```

### 5.2 get_awaiter 查找规则

编译器按以下顺序查找 `get_awaiter`：

```cpp
// 1. 成员 await_transform
if constexpr (requires { promise.await_transform(expr); }) {
    auto&& awaitable = promise.await_transform(expr);
    // 然后查找 awaitable 的 awaiter
}

// 2. 成员 operator co_await
if constexpr (requires { expr.operator co_await(); }) {
    auto&& awaiter = expr.operator co_await();
}

// 3. 全局 operator co_await
if constexpr (requires { operator co_await(expr); }) {
    auto&& awaiter = operator co_await(expr);
}

// 4. 直接使用 expr 作为 awaiter
else {
    auto&& awaiter = expr;
}
```

### 5.3 本项目的 Awaiter 实现

以 `YieldAwaiter` 为例（[coroutine.h:544](include/coro/coroutine.h#L544)）：

```cpp
class YieldAwaiter {
public:
    // 总是挂起
    bool await_ready() noexcept { return false; }

    // 挂起时：重新入队协程
    bool await_suspend(std::coroutine_handle<> h) noexcept {
        CoroutineMeta* meta = current_coro_meta();
        if (meta) {
            meta->state.store(CoroutineMeta::READY, std::memory_order_release);
            CoroutineScheduler::Instance().EnqueueCoroutine(meta);
            return true;  // 确认挂起
        }
        return false;  // 非调度器上下文，立即恢复
    }

    // 恢复时：无操作
    void await_resume() noexcept {}
};
```

**编译器转换后的执行流程**：

```cpp
// co_await yield();
{
    YieldAwaiter __awaiter = yield();  // 构造 awaiter

    if (!__awaiter.await_ready()) {    // 总是返回 false
        frame->__state_index = NEXT_STATE;

        bool should_suspend = __awaiter.await_suspend(handle);
        if (should_suspend) {
            return;  // 挂起，返回 Worker 线程循环
        }
        // 如果返回 false，继续执行
    }

    __awaiter.await_resume();  // 无操作
}
```

---

## 六、promise_type 约定

### 6.1 必需接口

```cpp
class promise_type {
public:
    // 创建返回对象
    Task get_return_object();

    // 初始挂起行为
    auto initial_suspend();

    // 最终挂起行为
    auto final_suspend() noexcept;

    // 处理返回值（二选一）
    void return_value(T);   // co_return value;
    void return_void();     // co_return; 或执行到函数末尾

    // 异常处理
    void unhandled_exception();
};
```

### 6.2 可选接口

```cpp
class promise_type {
public:
    // 自定义内存分配
    static void* operator new(size_t size);
    static void operator delete(void* ptr);

    // 转换 awaitable
    auto await_transform(auto&& expr);

    // 获取当前协程句柄
    std::coroutine_handle<> get_handle();
};
```

### 6.3 本项目 TaskPromise 实现

[coroutine.h:42-108](include/coro/coroutine.h#L42)：

```cpp
template<typename T>
class TaskPromise {
public:
    // 使用帧池分配
    static void* operator new(size_t size) {
        return GetGlobalFramePool().Allocate(size);
    }
    static void operator delete(void* ptr) {
        return GetGlobalFramePool().Deallocate(ptr);
    }

    // 创建 Task 对象
    Task<T> get_return_object() {
        return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
    }

    // 创建后挂起，等待调度器 resume
    std::suspend_always initial_suspend() noexcept { return {}; }

    // 完成时唤醒等待者
    FinalAwaiter final_suspend() noexcept { return {}; }

    // 存储 co_return 的值
    void return_value(T value) {
        result_ = std::move(value);
    }

    // 存储异常
    void unhandled_exception() {
        exception_ = std::current_exception();
    }

private:
    T result_{};
    std::exception_ptr exception_;
    std::coroutine_handle<> awaiter_;  // 等待此协程的调用者
};
```

---

## 七、FinalAwaiter 的关键作用

### 7.1 对称切换（Symmetric Transfer）

协程完成时，通过 `final_suspend` 返回的 awaiter 实现**零开销切换**：

```cpp
class FinalAwaiter {
public:
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<TaskPromise<T>> h) noexcept {
        // 返回等待者的句柄，直接切换过去
        if (h.promise().awaiter_) {
            return h.promise().awaiter_;  // 切换到等待者
        }
        return std::noop_coroutine();     // 无等待者，不切换
    }

    void await_resume() noexcept {}
};
```

### 7.2 执行流程

```
协程 A                    协程 B（等待 A）
    │                          │
    │  co_await task_a         │
    │─────────────────────────>│
    │  （B 挂起，记录等待者）   │
    │                          │
    │  ... 执行 A ...          │
    │                          │
    │  co_return value         │
    │                          │
    │  final_suspend()         │
    │  返回 B 的句柄           │
    │─────────────────────────>│
    │  （直接切换到 B）         │
    │                          │
    │                          │  await_resume()
    │                          │  获得结果
    │                          │  继续执行 B
```

**关键优势**：
- 无需返回调度器
- 无额外调度开销
- 栈不增长（无递归）

---

## 八、协程帧生命周期

### 8.1 完整生命周期

```
┌─────────────────────────────────────────────────────────────┐
│                     协程调用                                 │
│  example(x)                                                 │
│      │                                                      │
│      ├─> operator new 分配帧                                │
│      ├─> 构造 promise                                       │
│      ├─> 保存参数                                           │
│      ├─> get_return_object() 返回 Task                      │
│      ├─> initial_suspend()                                  │
│      │       └─> 挂起，返回给调用者                          │
└─────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│                     协程恢复                                 │
│  handle.resume()                                            │
│      │                                                      │
│      ├─> 查找 state_index                                   │
│      ├─> switch 跳转到对应状态                              │
│      ├─> 执行到下一个 co_await                              │
│      │       └─> 挂起，返回                                 │
│      └─> 或执行到 co_return                                 │
│              └─> return_value()                             │
│              └─> final_suspend()                            │
│                      └─> 切换到等待者或挂起                   │
└─────────────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│                     协程销毁                                 │
│  handle.destroy()                                           │
│      │                                                      │
│      ├─> 析构局部变量（逆序）                                │
│      ├─> 析构 promise                                       │
│      └─> operator delete 释放帧                             │
└─────────────────────────────────────────────────────────────┘
```

### 8.2 析构顺序

帧析构时，按**构造的逆序**析构：

```cpp
// 析构伪代码
void __coroutine_frame_destroy(__coroutine_frame_example* frame) {
    // 1. 析构局部变量（逆序）
    frame->__local_vec.~vector();
    // frame->__local_a 是 POD，无需析构

    // 2. 析构 awaiter 临时对象
    frame->__awaiter_1.~SleepAwaiter();
    frame->__awaiter_0.~YieldAwaiter();

    // 3. 析构参数副本
    frame->__param_y.~string();

    // 4. 析构 promise
    frame->__promise.~TaskPromise();

    // 5. 释放内存
    TaskPromise<int>::operator delete(frame);
}
```

---

## 九、编译器优化

### 9.1 帧分配优化

如果编译器能证明帧生命周期可预测，可能进行优化：

```cpp
// 可能的优化：栈上分配小帧
Task<int> small_coro() {
    co_return 42;
}

// 编译器可能优化为栈分配（如果调用者等待完成）
void caller() {
    auto task = small_coro();  // 帧可能在 caller 栈上
    task.handle().resume();    // 无堆分配
}
```

### 9.2 内联优化

简单协程可能被完全内联：

```cpp
Task<int> simple() {
    co_return 1 + 1;
}

// 可能优化为直接返回
int result = simple().get();  // 编译器可能直接计算为 2
```

### 9.3 空帧优化

如果协程没有跨挂起点的局部变量，帧可能更小：

```cpp
Task<void> empty_coro() {
    co_return;  // 无局部变量，帧很小
}

// 帧只需包含：
// - resume/destroy 函数指针
// - state_index
// - promise
```

---

## 十、调试视图

### 10.1 编译器生成的符号

使用 `objdump` 或调试器可以看到编译器生成的符号：

```bash
# Linux
objdump -t a.out | grep coroutine

# 输出类似：
# __coroutine_frame_example
# __example_resume
# __example_destroy
```

### 10.2 查看帧大小

```cpp
// 在 promise_type 中打印帧大小
static void* operator new(size_t size) {
    std::cout << "Frame size: " << size << " bytes\n";
    return ::operator new(size);
}

// 输出示例：
// Frame size: 128 bytes  (简单协程)
// Frame size: 2048 bytes (有大型局部变量)
```

### 10.3 本项目帧池

本项目使用 8KB 帧池（[coroutine.h:33](include/coro/coroutine.h#L33)）：

```cpp
inline FramePool& GetGlobalFramePool() {
    static FramePool pool;
    static std::once_flag init_flag;
    std::call_once(init_flag, [] { pool.Init(8 * 1024, 32); });
    return pool;
}
```

如果帧大小超过 8KB，会回退到堆分配。

---

## 十一、总结

### 编译器做了什么

| 步骤 | 编译器行为 |
|------|-----------|
| **解析协程** | 识别 `co_await`/`co_return`/`co_yield` |
| **计算帧大小** | 统计局部变量、参数、awaiter 大小 |
| **生成帧类型** | 创建协程帧结构体 |
| **生成状态机** | switch-case + 状态索引 |
| **插入转换代码** | co_await → await_ready/suspend/resume |
| **调用 promise 接口** | get_return_object、return_value 等 |

### 程序员的责任

| 接口 | 责任 |
|------|------|
| `promise_type` | 定义协程行为（返回类型、挂起策略、异常处理） |
| `Awaiter` | 定义 `co_await` 行为（何时挂起、挂起/恢复时做什么） |
| `Task` | 管理协程句柄生命周期 |

### 关键洞察

C++20 协程的本质是**编译器辅助的状态机生成**：
- 协程帧 = 状态机状态存储
- resume() = 状态机步进
- co_await = 状态转换点

理解这个转换过程，就能更好地设计和调试协程代码。