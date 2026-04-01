# 线程栈切换详解

## 核心概念

栈切换是实现用户级线程的关键技术——通过保存/恢复 CPU 上下文，在同一个内核线程中切换执行不同的 bthread。

---

## 1. Context 数据结构

[platform.h:12](include/bthread/platform/platform.h#L12) 定义了上下文结构：

```cpp
struct Context {
    union {
        uint64_t gp_regs[16];     // 通用寄存器
        void* ptr_regs[16];
    };

    alignas(16) uint8_t xmm_regs[160];  // XMM 寄存器

    void* stack_ptr;      // 栈指针
    void* return_addr;    // 返回地址
};
```

**内存布局**：
```
偏移 0-127:   gp_regs[16]     (128 bytes)
偏移 128-287: xmm_regs[160]   (160 bytes, Windows 需要)
偏移 288:     stack_ptr       (8 bytes)
偏移 296:     return_addr     (8 bytes)
```

---

## 2. 需保存的寄存器

根据 ABI 规范，只需保存 **callee-saved 寄存器**：

| 平台 | Callee-saved 寄存器 |
|------|---------------------|
| **Linux x64 (System V)** | rbx, rbp, r12-r15 (6 个) |
| **Windows x64 (Microsoft)** | rbx, rbp, rsi, rdi, r12-r15, **xmm6-xmm15** (8 GPR + 10 XMM) |

**caller-saved 寄存器**（rax, rcx, rdx, r8-r11 等）由调用者保存，切换后会被覆盖，无需保存。

---

## 3. SwapContext 实现

### Windows 版本 ([context_windows_x64.asm](src/platform/context_windows_x64.asm))

```asm
SwapContext PROC
    ; rcx = from (保存当前上下文)
    ; rdx = to   (恢复目标上下文)

    ; 1. 保存 GPR
    mov [rcx + 0*8], rbx
    mov [rcx + 1*8], rbp
    mov [rcx + 2*8], rsi
    mov [rcx + 3*8], rdi
    mov [rcx + 4*8], r12-r15

    ; 2. 保存 XMM (Windows 特有！)
    movdqa xmmword ptr [rcx + 128], xmm6
    ... (xmm7-xmm15)

    ; 3. 保存返回地址和栈指针
    mov rax, [rsp]           ; 返回地址
    lea r11, [rsp + 8]       ; 调用后的 rsp
    mov [rcx + 288], r11     ; 保存 stack_ptr
    mov [rcx + 296], rax     ; 保存 return_addr

    ; 4. 加载目标上下文
    mov rbx-r15, [rdx + ...]
    movdqa xmm6-15, [rdx + 128...]

    ; 5. 切换栈
    mov rsp, [rdx + 288]

    ; 6. 跳转到返回地址
    mov rax, [rdx + 296]
    jmp rax                  ; 跳转而非 ret！
SwapContext ENDP
```

### Linux 版本

```asm
SwapContext:
    ; rdi = from, rsi = to

    ; 保存 callee-saved
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12-r15, 16-40(%rdi)
    movq %rsp, 112(%rdi)      ; 栈指针

    ; 加载目标
    movq 0(%rsi), %rbx...
    movq 112(%rsi), %rsp      ; 切换栈！

    ret                        ; 返回到新上下文
```

**关键区别**：
- Linux 用 `ret`（返回地址已在目标栈上）
- Windows 用 `jmp`（显式加载返回地址）

---

## 4. MakeContext 初始化

创建新 bthread 时，设置初始栈帧：

### Windows 版本

```asm
MakeContext PROC
    ; rcx=ctx, rdx=stack_top, r8=stack_size, r9=fn, [rsp+40]=arg

    mov r10, [rsp + 40]       ; 取 arg

    ; 栈布局：
    ; [rsp+8]  = fn
    ; [rsp+16] = arg
    sub rax, 32

    mov [rax + 8], r9         ; 存 fn
    mov [rax + 16], r10       ; 存 arg

    mov [rcx + 288], rax      ; 设置 stack_ptr
    lea rax, BthreadStart
    mov [rcx + 296], rax      ; 设置 return_addr 为入口
MakeContext ENDP

BthreadStart PROC
    mov rcx, [rsp + 16]       ; arg → rcx (Windows 第一参数)
    mov rax, [rsp + 8]        ; fn
    add rsp, 8                ; 调整栈对齐
    jmp rax                   ; 跳转到用户函数
BthreadStart ENDP
```

### Linux 版本

```asm
MakeContext:
    ; rdi=ctx, rsi=stack, rdx=stack_size, rcx=fn, r8=arg

    ; 计算 16 字节对齐的栈顶
    movq %rsi, %rax
    addq %rdx, %rax
    andq $-16, %rax

    subq $8, %rax             ; 留空间给返回地址
    movq %rcx, (%rax)         ; fn 当作"返回地址"

    movq %rax, 112(%rdi)      ; 设置 stack_ptr
    movq %r8, 0(%rdi)         ; arg → gp_regs[0] (将来会加载到 rdi)
MakeContext:
```

---

## 5. 栈布局图解

**创建 bthread 时**：
```
高地址
┌─────────────────────┐ ← stack_top (AllocateStack 返回)
│   Guard Page        │  ← 栈溢出保护 (PAGE_NOACCESS)
├─────────────────────┤
│                     │
│   Stack Space       │  ← 1MB 可用空间
│                     │
├─────────────────────┤
│   [fn]              │  ← 将作为"返回地址"
│   [arg]             │  ← 参数
│   [BthreadStart]    │  ← Windows 入口包装
└─────────────────────┘ ← 初始 rsp
低地址
```

**运行时切换**：
```
Worker 线程                      bthread
    │                              │
    │  SwapContext(&saved, &task)  │
    │─────────────────────────────>│
    │                              │
    │  保存 rbx, rbp, r12-r15      │
    │  保存 rsp → saved.stack_ptr  │
    │  加载 task.stack_ptr → rsp   │  ← 栈切换！
    │  jmp return_addr             │
    │                              │ 执行用户函数
    │                              │ fn(arg)
    │                              │
    │  SwapContext(&task, &saved)  │
    │<─────────────────────────────│
    │                              │
    │  恢复 rsp                     │
    │  继续 Worker 主循环           │
```

---

## 6. 实际调用链

[worker.cpp:44](src/worker.cpp#L44) 展示了切换过程：

```cpp
void Worker::Run() {
    while (running) {
        TaskMeta* task = PickTask();
        current_task_ = task;

        // 切换到 bthread 执行
        platform::SwapContext(&saved_context_, &task->context);

        // 从 bthread 返回
        HandleTaskAfterRun(task);
    }
}
```

[bthread.cpp](src/bthread.cpp) 中的入口：

```cpp
static void BthreadEntry(void* arg) {
    TaskMeta* task = static_cast<TaskMeta*>(arg);
    task->result = task->fn(task->arg);  // 执行用户函数
    bthread_exit(task->result);          // 切回 Worker
}

void bthread_exit(void* retval) {
    Worker* w = Worker::Current();
    TaskMeta* task = w->current_task();
    task->state = TaskState::FINISHED;

    // 切回 Worker，永不返回
    platform::SwapContext(&task->context, &w->saved_context_);
}
```

---

## 7. 栈分配与保护

[platform_windows.cpp:35](src/platform/platform_windows.cpp#L35)：

```cpp
void* AllocateStack(size_t size) {
    size_t total = size + PAGE_SIZE * STACK_GUARD_PAGES;

    void* ptr = VirtualAlloc(..., PAGE_READWRITE);

    // 设置 Guard Page 为不可访问
    VirtualProtect(ptr, PAGE_SIZE * 2, PAGE_NOACCESS, &old);

    // 返回栈顶（最高地址，16 字节对齐）
    void* stack_top = (char*)ptr + total;
    stack_top &= ~0xF;
    return stack_top;
}
```

**Guard Page 作用**：栈溢出时访问不可访问页面，触发 SIGSEGV/EXCEPTION_STACK_OVERFLOW，防止损坏其他内存。

---

## 8. 性能特点

| 特性 | 说明 |
|------|------|
| **纯用户态** | 无系统调用，切换耗时 < 100ns |
| **无内核参与** | 内核只知道 pthread，不感知 bthread |
| **栈复用** | TaskMeta 池化，栈内存不反复分配释放 |
| **缓存友好** | 上下文结构紧凑，寄存器保存在连续内存 |

---

## 总结

栈切换的本质是：
1. **保存当前执行点**（寄存器 + 栈指针 + 返回地址）
2. **加载目标执行点**
3. **跳转到目标代码继续执行**

这实现了在单个 pthread 上"时分复用"执行多个 bthread，是 M:N 模型的核心技术。