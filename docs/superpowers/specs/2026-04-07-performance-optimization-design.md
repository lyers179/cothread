# bthread 性能优化设计 v3

**日期**: 2026-04-07
**目标**: 提升任务调度效率、同步原语性能和多核扩展性

---

## 概述

优化三大核心瓶颈：
1. **Butex无锁等待队列** - 消除`queue_mutex_`锁竞争
2. **XMM寄存器惰性保存** - 减少上下文切换开销
3. **WorkStealingQueue缓存优化** - padding + 批处理

---

## 性能目标

| 指标 | 当前 | 目标 | 提升比例 |
|------|------|------|----------|
| Mutex竞争 | ~6M ops/sec | 15M+ | 2.5x |
| Yield性能 | ~35M yields/sec | 45M+ | 1.3x |
| 多核扩展性 | 亚线性 | 接近线性 | - |

---

## 设计一：Butex无锁等待队列（Dmitry Vyukov MPSC）

### 问题

当前`Butex`使用`queue_mutex_`保护等待队列，高并发时锁竞争严重：
- `AddToTail/AddToHead` 阻塞
- `Wait()` 多次lock/unlock周期

### 解决方案

使用Dmitry Vyukov的MPSC无锁队列算法，配合正确的状态机防止ABA问题。

### 数据结构

```cpp
// 等待队列节点（内联在TaskMeta中）
struct WaiterNode {
    std::atomic<WaiterNode*> next{nullptr};
    std::atomic<bool> claimed{false};  // 防止重复消费
};

// TaskMeta扩展
struct TaskMeta {
    // ... 现有字段

    // 等待队列状态
    std::atomic<bool> is_waiting{false};  // 是否正在等待（防止ABA）
    WaiterNode waiter_node;               // 预分配节点，避免动态分配
};

// Butex队列
class Butex {
    std::atomic<WaiterNode*> head_{nullptr};
    std::atomic<WaiterNode*> tail_{nullptr};
    // 移除 queue_mutex_
};
```

### Wait() 状态机

关键：使用`is_waiting`作为状态锁，防止Wait和Wake之间的竞态：

```cpp
int Butex::Wait(int expected_value, const timespec* timeout, bool prepend) {
    Worker* w = Worker::Current();
    if (!w || w->current_task()->type != TaskType::BTHREAD) {
        return FutexWait(&value_, expected_value, timeout);
    }

    TaskMeta* task = static_cast<TaskMeta*>(w->current_task());

    // 1. Check value first
    if (value_.load(std::memory_order_acquire) != expected_value) {
        return 0;
    }

    // 2. Mark as "about to enter queue" - prevent concurrent Wake
    bool expected = false;
    if (!task->is_waiting.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // Already waiting - task is being consumed, return immediately
        return 0;
    }

    // 3. Prepare waiter node
    WaiterNode* node = &task->waiter_node;
    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    // 4. Double-check value after setting is_waiting
    if (value_.load(std::memory_order_acquire) != expected_value) {
        // Value changed, remove ourselves
        task->is_waiting.store(false, std::memory_order_release);
        return 0;
    }

    // 5. Set up timeout
    if (timeout) {
        // ... existing timeout setup
    }

    // 6. Record which butex we're waiting on
    task->waiting_butex = this;

    // 7. Add to queue (lock-free MPSC)
    if (prepend) {
        AddToHead(task);
    } else {
        AddToTail(task);
    }

    // 8. Set state to SUSPENDED
    task->state.store(TaskState::SUSPENDED, std::memory_order_release);

    // 9. Check if Wake already happened - must check BOTH is_waiting and state
    if (!task->is_waiting.load(std::memory_order_acquire)) {
        // Wake cleared is_waiting, check if it transitioned state
        TaskState state = task->state.load(std::memory_order_acquire);
        if (state != TaskState::SUSPENDED) {
            // Wake already set us to READY or other state, don't suspend
            task->waiting_butex = nullptr;
            return 0;
        }
        // is_waiting cleared but state still SUSPENDED - Wake is in progress
        // Continue to suspend - Wake will set us READY
    }

    // 10. Suspend
    w->SuspendCurrent();

    // 11. Resumed
    task->waiting_butex = nullptr;

    if (ws.timed_out.load(std::memory_order_acquire)) {
        return ETIMEDOUT;
    }
    return 0;
}
```

### AddToTail (Producer - FIFO)

```cpp
void Butex::AddToTail(TaskMeta* task) {
    // Verify task is still supposed to be in queue
    if (!task->is_waiting.load(std::memory_order_relaxed)) {
        // Task already removed by Wake, don't add
        return;
    }

    WaiterNode* node = &task->waiter_node;
    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    // Exchange tail - acq_rel provides full barrier
    WaiterNode* prev = tail_.exchange(node, std::memory_order_acq_rel);
    if (prev) {
        // Link previous node to new node
        prev->next.store(node, std::memory_order_release);
    } else {
        // Queue was empty, also set head
        WaiterNode* expected = nullptr;
        head_.compare_exchange_strong(expected, node,
            std::memory_order_release, std::memory_order_relaxed);
    }
}
```

### AddToHead (Producer - LIFO for first-time waiters)

**注意**: `prepend=true`用于首次等待（官方bthread的first_wait=LIFO优化），不用于re-queue。

```cpp
void Butex::AddToHead(TaskMeta* task) {
    // Verify task is still supposed to be in queue
    if (!task->is_waiting.load(std::memory_order_relaxed)) {
        // Task already removed by Wake, don't add
        return;
    }

    WaiterNode* node = &task->waiter_node;
    // Don't set claimed=true - Wait() already initialized it to false

    // Use CAS loop for head insertion
    while (true) {
        WaiterNode* old_head = head_.load(std::memory_order_acquire);

        // Double-check is_waiting in the loop (Wake could have cleared it)
        if (!task->is_waiting.load(std::memory_order_relaxed)) {
            return;
        }

        node->next.store(old_head, std::memory_order_relaxed);

        if (head_.compare_exchange_strong(old_head, node,
                std::memory_order_release, std::memory_order_relaxed)) {
            // Successfully inserted at head
            // If this was the first node, also update tail
            if (!old_head) {
                WaiterNode* expected = nullptr;
                tail_.compare_exchange_strong(expected, node,
                    std::memory_order_release, std::memory_order_relaxed);
            }
            return;
        }
        // CAS failed, retry
    }
}
```

### PopFromHead (Consumer) - 修复内存屏障

**关键修复**: 使用CAS with acq_rel提供正确的同步屏障

```cpp
TaskMeta* Butex::PopFromHead() {
    while (true) {
        // Load head with acquire
        WaiterNode* head = head_.load(std::memory_order_acquire);
        if (!head) return nullptr;

        // Try to claim this node
        if (head->claimed.exchange(true, std::memory_order_acq_rel)) {
            // Already claimed, skip
            head = head->next.load(std::memory_order_acquire);
            continue;
        }

        // Load next with relaxed - the CAS below provides the barrier
        WaiterNode* next = head->next.load(std::memory_order_relaxed);

        // Try to advance head - acq_rel provides synchronization for accessing head->task
        WaiterNode* expected = head;
        if (head_.compare_exchange_strong(expected, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Successfully claimed and advanced
            return static_cast<TaskMeta*>(
                reinterpret_cast<char*>(head) - offsetof(TaskMeta, waiter_node));
        }

        // CAS failed, reset claimed and retry
        head->claimed.store(false, std::memory_order_relaxed);
    }
}
```

### RemoveFromWaitQueue

```cpp
void Butex::RemoveFromWaitQueue(TaskMeta* task) {
    // First mark as not waiting atomically
    bool expected = true;
    if (!task->is_waiting.compare_exchange_strong(expected, false,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;  // Already removed or not in queue
    }

    // Mark node as claimed so PopFromHead will skip it
    task->waiter_node.claimed.store(true, std::memory_order_release);

    // Note: We don't actually remove from linked structure
    // PopFromHead will handle it when it reaches this node
}
```

### Wake()

```cpp
void Butex::Wake(int count) {
    // Wake futex waiters (pthreads)
    FutexWake(&value_, count);

    int woken = 0;
    while (woken < count) {
        TaskMeta* waiter = PopFromHead();
        if (!waiter) break;

        // Clear is_waiting - task is no longer waiting
        waiter->is_waiting.store(false, std::memory_order_release);

        // Cancel pending timeout
        if (waiter->waiter.timer_id != 0) {
            Scheduler::Instance().GetTimerThread()->Cancel(waiter->waiter.timer_id);
        }

        // Check if task is SUSPENDED
        TaskState state = waiter->state.load(std::memory_order_acquire);
        if (state == TaskState::SUSPENDED) {
            waiter->state.store(TaskState::READY, std::memory_order_release);
            Scheduler::Instance().EnqueueTask(waiter);
            ++woken;
        }
        // If not SUSPENDED, the task is still preparing to suspend
        // It will check is_waiting and return without suspending
    }
}
```

### 内存屏障策略总结

| 操作 | 屏障类型 | 原因 |
|------|---------|------|
| `tail_.exchange` | `acq_rel` | 建立producer/consumer同步，确保prev->next可见 |
| `prev->next.store` | `release` | 发布新节点 |
| `head_.compare_exchange` | `acq_rel` | 推进head，同时提供访问head->data的屏障 |
| `is_waiting.*` | `acq_rel` | 防止Wait/Wake竞态 |
| `claimed.exchange` | `acq_rel` | 防止重复消费 |

---

## 设计二：XMM寄存器惰性保存

### 问题

当前`SwapContext`总是保存xmm6-xmm15（10个寄存器，160字节），即使任务不使用SIMD。

### 解决方案

任务级`uses_xmm`标志，首次使用时通过运行时检测设置。

### TaskMeta改动

```cpp
struct TaskMeta {
    // ... 现有字段
    bool uses_xmm{false};  // 标记任务是否使用过XMM寄存器
};

// 常量：uses_xmm在Context中的偏移
// Context layout: gp_regs[16] (128B) + xmm_regs[160] (160B) + stack_ptr (8B) + return_addr (8B)
// uses_xmm存储在TaskMeta中，不在Context内
// 需要通过TaskMeta地址访问
```

### SwapContext改动 - 运行时检测

```asm
; SwapContext(from, to)
; rcx = from, rdx = to
; 注意: from/to是TaskMeta*，Context在TaskMeta内部

; 计算TaskMeta中context的偏移 (假设context_offset为已知常量)
; 需要通过rdx (TaskMeta*) + context_offset访问context
; 这里简化为直接传递context指针

; 实际实现中，SwapContext接收Context*，TaskMeta的uses_xmm需要额外参数或通过全局变量获取

; 方案：SwapContext额外接收uses_xmm指针作为第三个参数
; SwapContext(from, to, to_uses_xmm_ptr)
; rcx = from, rdx = to, r8 = to_uses_xmm_ptr

; ============== 保存部分 ==============

; 检查to_uses_xmm
test    r8, r8
jz      save_xmm_zero_detect

; 有uses_xmm指针，检查标志
mov     r9, [r8]
test    r9, r9
jnz     save_xmm_now

; 首次：检测xmm6-xmm15是否非零
pxor    xmm0, xmm0
por     xmm0, xmm6
por     xmm0, xmm7
por     xmm0, xmm8
por     xmm0, xmm9
por     xmm0, xmm10
por     xmm0, xmm11
por     xmm0, xmm12
por     xmm0, xmm13
por     xmm0, xmm14
por     xmm0, xmm15

ptest   xmm0, xmm0
jz      save_xmm_skip

; 至少一个xmm非零，设置uses_xmm
mov     byte ptr [r8], 1

save_xmm_now:
; 保存xmm6-xmm15
movdqa  [rcx + 128], xmm6
movdqa  [rcx + 144], xmm7
movdqa  [rcx + 160], xmm8
movdqa  [rcx + 176], xmm9
movdqa  [rcx + 192], xmm10
movdqa  [rcx + 208], xmm11
movdqa  [rcx + 224], xmm12
movdqa  [rcx + 240], xmm13
movdqa  [rcx + 256], xmm14
movdqa  [rcx + 272], xmm15
jmp     save_xmm_done

save_xmm_zero_detect:
; 没有uses_xmm指针，检测xmm并假设存储在from的TaskMeta中
; 这需要更复杂的指针计算
; 简化方案：总是检测并保存（向后兼容）
pxor    xmm0, xmm0
; ... 同上检测
ptest   xmm0, xmm0
jz      save_xmm_skip
; 保存xmm

save_xmm_skip:
save_xmm_done:
; ... 保存GPRs ...

; ============== 加载部分 ==============

; 检查to_uses_xmm
test    r8, r8
jz      load_xmm_always  ; 无指针，总是加载

mov     r9, [r8]
test    r9, r9
jz      load_xmm_skip

load_xmm_now:
; 加载xmm6-xmm15
movdqa  xmm6, [rdx + 128]
movdqa  xmm7, [rdx + 144]
movdqa  xmm8, [rdx + 160]
movdqa  xmm9, [rdx + 176]
movdqa  xmm10, [rdx + 192]
movdqa  xmm11, [rdx + 208]
movdqa  xmm12, [rdx + 224]
movdqa  xmm13, [rdx + 240]
movdqa  xmm14, [rdx + 256]
movdqa  xmm15, [rdx + 272]
jmp     load_xmm_done

load_xmm_skip:
load_xmm_always:
; 向后兼容：没有uses_xmm时总是加载

load_xmm_done:
; ... 继续跳转
```

### 调用约定修改

```cpp
// platform/platform.h
namespace platform {
    void SwapContext(Context* from, Context* to, bool* to_uses_xmm = nullptr);
}
```

```cpp
// Worker::RunBthread
void Worker::RunBthread(TaskMeta* task) {
    platform::SwapContext(&saved_context_, &task->context, &task->uses_xmm);
}
```

### MakeContext改动

```asm
MakeContext:
    ; MakeContext(ctx, stack_top, stack_size, fn, arg)
    ; rcx=ctx, rdx=stack_top, r8=stack_size, r9=fn, [rsp+40]=arg

    ; Zero xmm region in context for initial state
    ; This enables runtime detection
    lea     rax, [rcx + 128]  ; xmm_regs offset
    mov     rcx, 20           ; 160 bytes / 8 = 20 qwords
    xor     rdx, rdx
zero_xmm_loop:
    mov     [rax], rdx
    add     rax, 8
    loop    zero_xmm_loop

    ; ... rest of MakeContext ...
```

---

## 设计三：WorkStealingQueue缓存优化

### 问题1：False Sharing

`head_`和`tail_`在同一cache line，多worker竞争时互相失效。

### 解决方案1：alignas Padding

```cpp
class WorkStealingQueue {
    static constexpr size_t CACHE_LINE_SIZE = 64;

    // buffer不需要alignas，因为很大且只被owner访问
    std::atomic<TaskMetaBase*> buffer_[CAPACITY];

    // head独占cache line
    alignas(CACHE_LINE_SIZE)
    std::atomic<uint64_t> head_{0};

    // tail独占cache line
    alignas(CACHE_LINE_SIZE)
    std::atomic<uint64_t> tail_{0};
};
```

**关键**: `alignas`确保每个变量从cache line边界开始，比手动padding更可靠。

### 问题2：频繁队列操作

每次`Push/Pop`都涉及原子操作和cache line同步。

### 解决方案2：Worker批处理

```cpp
class Worker {
    static constexpr int BATCH_SIZE = 8;

    TaskMetaBase* local_batch_[BATCH_SIZE];
    int batch_count_{0};

    void MaybeFlushBatch() {
        if (batch_count_ >= BATCH_SIZE) {
            for (int i = 0; i < batch_count_; ++i) {
                local_queue_.Push(local_batch_[i]);
            }
            batch_count_ = 0;
        }
    }

    TaskMetaBase* PickTask() {
        // 1. 从batch取（LIFO，符合局部性）
        if (batch_count_ > 0) {
            return local_batch_[--batch_count_];
        }

        // 2. 从local queue取，批量填充batch
        if (auto t = local_queue_.Pop()) {
            local_batch_[batch_count_++] = t;
            // 预取更多
            for (int i = 0; i < BATCH_SIZE - 1 && batch_count_ < BATCH_SIZE; ++i) {
                if (auto t2 = local_queue_.Pop()) {
                    local_batch_[batch_count_++] = t2;
                } else {
                    break;
                }
            }
            return local_batch_[--batch_count_];
        }

        // 3. Global queue
        if (auto t = Scheduler::Instance().global_queue().Pop()) {
            return t;
        }

        // 4. Work stealing
        int32_t wc = Scheduler::Instance().worker_count();
        if (wc <= 1) return nullptr;

        int attempts = wc * 3;
        static thread_local std::mt19937 rng(std::random_device{}());

        for (int i = 0; i < attempts; ++i) {
            int victim = (id_ + rng()) % wc;
            if (victim == id_) continue;

            Worker* other = Scheduler::Instance().GetWorker(victim);
            if (other) {
                if (auto t = other->local_queue_.Steal()) {
                    return t;
                }
            }
        }

        return nullptr;
    }

    void YieldCurrent() {
        current_task_->state.store(TaskState::READY, std::memory_order_release);

        // 添加到batch
        local_batch_[batch_count_++] = current_task_;

        // 检查是否需要flush
        if (batch_count_ >= BATCH_SIZE) {
            MaybeFlushBatch();
        }

        SuspendCurrent();
    }
};
```

### FIFO讨论

**注意**: 批处理在batch内部使用LIFO（栈），这改变了任务执行顺序：
- 任务A、B、C依次yield放入batch
- PickTask先返回C，然后B，最后A

**影响评估**:
- 对于短任务：影响可忽略，LIFO更好的cache局部性
- 对于有顺序要求的任务：可能违反预期
- Work stealing从队列头部取，保持公平性

**如果需要严格的FIFO**:
- 使用循环缓冲区代替stack
- 或禁用批处理（BATCH_SIZE=0）

---

## 测试策略

### 新增单元测试

**`tests/mpsc_queue_test.cpp`**:
```cpp
// 单生产者单消费者正确性
TEST(MPSCQueueTest, SingleProducerSingleConsumer) {
    // ... 实现MPSC队列
    // 验证所有任务都被消费
}

// 单生产者多消费者并发
TEST(MPSCQueueTest, SingleProducerMultiConsumer) {
    // 16个消费者线程竞争Pop
    // 验证没有任务丢失或重复消费
}

// Wait/Wake竞态条件测试
TEST(MPSCQueueTest, WaitWakeRaceCondition) {
    // 精确控制Wait和Wake的时序
    // 创建竞态窗口
    // 验证没有崩溃或错误状态
}

// Double-queue测试（防御性）
TEST(MPSCQueueTest, DoubleQueueAttempt) {
    // 尝试将同一任务加入多个Butex队列
    // 验证只有第一个成功
}

// 内存泄漏检测
TEST(MPSCQueueTest, MemoryLeak) {
    // 运行大量Wait/Wake操作
    // 检查内存是否释放
}
```

**`tests/xmm_test.cpp`**:
```cpp
// SIMD使用验证
TEST(XMMLazyTest, SIMDUsageDetected) {
    __m128 a = _mm_set_ps(1, 2, 3, 4);
    __m128 b = _mm_set_ps(5, 6, 7, 8);
    __m128 c = _mm_mul_ps(a, b);  // 使用xmm6-xmm15
    bthread_yield();
    __m128 d = _mm_add_ps(c, a);  // 验证值未损坏
}

// 非SIMD任务验证
TEST(XMMLazyTest, NonSIMDSkipsSave) {
    // 不使用SIMD的任务
    bthread_yield();
    // 验证uses_xmm仍为false
}

// 混合场景压力测试
TEST(XMMLazyTest, MixedSIMDNonSIMD) {
    // 创建混合任务
    // 部分使用SIMD，部分不使用
    // 验证所有任务正确执行
}
```

**`tests/worker_batch_test.cpp`**:
```cpp
// Batch边界测试
TEST(WorkerBatchTest, BatchBoundaries) {
    // 测试batch_count达到BATCH_SIZE时的行为
}

// FIFO顺序验证（如果需要）
TEST(WorkerBatchTest, FIFOPreservation) {
    // 如果使用FIFO batch，验证顺序
}

// Work stealing与batch交互
TEST(WorkerBatchTest, StealingDuringBatch) {
    // worker的batch有任务时，其他worker尝试steal
    // 验证steal从queue取，不影响batch
}

// Batch underflow防御
TEST(WorkerBatchTest, BatchUnderflow) {
    // 测试异常情况
    // 添加断言防止batch_count < 0
}
```

### 回归测试

- 所有现有测试必须100%通过
- 添加多线程压力测试（16-32线程）
- 添加TSAN（ThreadSanitizer）检测数据竞态

### 性能基准

在`benchmark.cpp`中添加：

```cpp
// 高并发Mutex竞争测试
void benchmark_mutex_high_contention(int num_threads) {
    // 16-32线程竞争同一mutex
}

// XMM保存开销测试
void benchmark_xmm_overhead() {
    // 对比SIMD任务和非SIMD任务的性能
}

// 多核扩展性详细测试
void benchmark_scalability_detailed() {
    // 测试1, 2, 4, 8, 16, 32核
    // 验证接近线性的扩展性
}

// MPSC队列基准
void benchmark_mpsc_queue() {
    // 单生产者多消费者吞吐量
}
```

目标性能验证（相对提升）：
- Mutex: 2.5x提升（16线程）
- Yield: 1.3x提升
- 多核扩展: 8核接近8x

---

## 实施顺序

### Phase 1: Butex无锁队列（最重要，风险最高）

1. 修改`TaskMeta`添加`waiter_node`和`is_waiting`
2. 实现`Butex`的MPSC队列方法（AddToTail, AddToHead, PopFromHead）
3. 修改`Wait()`使用正确的状态机
4. 修改`Wake()`和`RemoveFromWaitQueue()`
5. 移除`queue_mutex_`
6. 单元测试验证
7. 运行所有现有测试

### Phase 2: WorkStealingQueue padding

1. 添加`CACHE_LINE_SIZE`常量
2. 使用`alignas`修改`WorkStealingQueue`
3. 验证cache line对齐（静态断言）
4. 性能测试验证

### Phase 3: Worker批处理

1. 添加batch字段到`Worker`
2. 修改`PickTask`使用batch
3. 修改`YieldCurrent`使用batch
4. 单元测试验证
5. 性能测试验证

### Phase 4: XMM惰性保存

1. 修改`TaskMeta`添加`uses_xmm`
2. 修改`SwapContext`签名添加uses_xmm参数
3. 修改汇编文件添加运行时检测逻辑
4. 修改`MakeContext`清零xmm区域
5. 修改`Worker::RunBthread`传递uses_xmm指针
6. 单元测试验证
7. 性能测试验证

---

## 风险与缓解

| 风险 | 缓解措施 |
|------|----------|
| MPSC ABA问题 | `is_waiting`状态机 + `claimed`标记 + 正确内存屏障 |
| MPSC队列starvation | 已有FIFO公平性，producer为单线程无starvation |
| XMM保存错误 | 运行时检测 + 调试时验证保存的值 |
| FIFO顺序破坏 | Batch内LIFO提供更好的局部性，如需严格FIFO可禁用batch |
| Padding失效 | 使用`alignas`，运行时检测cache line大小 |
| Batch溢出 | 添加边界检查和断言 |

### 运行时监控（可选）

```cpp
struct PerformanceMetrics {
    std::atomic<uint64_t> xmm_saves{0};       // XMM保存次数
    std::atomic<uint64_t> xmm_skips{0};       // XMM跳过次数
    std::atomic<uint64_t> batch_flushes{0};   // Batch flush次数
    std::atomic<uint64_t> butex_waits{0};     // Butex等待次数
    std::atomic<uint64_t> butex_wakes{0};     // Butex唤醒次数
};
```

---

## 参考资料

- Dmitry Vyukov's MPSC queue: http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
- False sharing: https://en.wikipedia.org/wiki/False_sharing
- Intel optimization manual: XMM寄存器保存开销分析
- C++ memory ordering: https://en.cppreference.com/w/cpp/atomic/memory_order