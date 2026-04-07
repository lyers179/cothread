# bthread 性能优化设计

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

使用Dmitry Vyukov的MPSC无锁队列算法。

### 数据结构

```cpp
// 新增等待队列节点
struct WaiterNode {
    std::atomic<WaiterNode*> next;
    TaskMeta* task;
    std::atomic<bool> claimed;  // 防止重复消费
};

// TaskMeta预分配节点
struct TaskMeta {
    // ... 现有字段
    WaiterNode waiter_node;  // 内联节点，避免动态分配
};

// Butex队列
class Butex {
    std::atomic<WaiterNode*> head_{nullptr};
    std::atomic<WaiterNode*> tail_{nullptr};
    // 移除 queue_mutex_
};
```

### 关键算法

**AddToTail (Producer)**:
```cpp
void Butex::AddToTail(TaskMeta* task) {
    WaiterNode* node = &task->waiter_node;
    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    WaiterNode* prev = tail_.exchange(node, std::memory_order_acq_rel);
    if (prev) {
        prev->next.store(node, std::memory_order_release);
    } else {
        // 队列为空，head也指向node
        WaiterNode* expected = nullptr;
        head_.compare_exchange_strong(expected, node,
            std::memory_order_release, std::memory_order_relaxed);
    }
}
```

**PopFromHead (Consumer)**:
```cpp
TaskMeta* Butex::PopFromHead() {
    WaiterNode* head = head_.load(std::memory_order_acquire);
    while (head) {
        // 尝试标记为已消费
        if (head->claimed.exchange(true, std::memory_order_acq_rel)) {
            head = head->next.load(std::memory_order_acquire);
            continue;
        }

        // 推进head
        WaiterNode* next = head->next.load(std::memory_order_acquire);
        head_.store(next, std::memory_order_release);

        return head->task;
    }
    return nullptr;
}
```

**AddToHead**: 简化为头插，复用MPSC结构

**RemoveFromWaitQueue**: 标记claimed，PopFromHead会跳过

### 内存屏障策略

- `tail_.exchange`: `acq_rel` - 确保prev的next可见
- `next.store`: `release` - 发布节点
- `claimed.exchange`: `acq_rel` - 防止竞态

---

## 设计二：XMM寄存器惰性保存

### 问题

当前`SwapContext`总是保存xmm6-xmm15（10个寄存器，160字节），即使任务不使用SIMD。

### 解决方案

任务级`uses_xmm`标志，首次使用时设置。

### TaskMeta改动

```cpp
struct TaskMeta {
    // ... 现有字段
    bool uses_xmm{false};
};
```

### SwapContext改动

```asm
; SwapContext(from, to)
; rcx = from, rdx = to

; 检查to->uses_xmm
mov     r8, [rdx + <uses_xmm_offset>]
test    r8, r8
jz      skip_save_xmm

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

skip_save_xmm:
; ... 继续GPR保存

; 加载GPRs后，检查uses_xmm
mov     r8, [rdx + <uses_xmm_offset>]
test    r8, r8
jz      skip_load_xmm

; 加载xmm6-xmm15
movdqa  xmm6, [rdx + 128]
; ...

skip_load_xmm:
; ... 继续跳转
```

### uses_xmm设置策略

**简化方案**: 初始化为`false`，首次Swap后保持原样（不自动检测）

**运行时检测**（可选）:
- MakeContext时清零xmm区域
- Swap时检测 xmm 值是否为0，非0则设置`uses_xmm`

---

## 设计三：WorkStealingQueue缓存优化

### 问题1：False Sharing

`head_`和`tail_`在同一cache line，多worker竞争时互相失效。

### 解决方案1：Padding

```cpp
class WorkStealingQueue {
    static constexpr size_t CACHE_LINE_SIZE = 64;

    alignas(CACHE_LINE_SIZE)
    std::atomic<TaskMetaBase*> buffer_[CAPACITY];

    // head独占cache line
    alignas(CACHE_LINE_SIZE)
    std::atomic<uint64_t> head_{0};

    // 填充
    char pad1_[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];

    // tail独占cache line
    alignas(CACHE_LINE_SIZE)
    std::atomic<uint64_t> tail_{0};
};
```

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
        // 1. 从batch取
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
        // ... 现有逻辑
    }

    void YieldCurrent() {
        current_task_->state.store(TaskState::READY, std::memory_order_release);
        // 放入batch而不是直接Push
        if (batch_count_ < BATCH_SIZE) {
            local_batch_[batch_count_++] = current_task_;
        } else {
            MaybeFlushBatch();
            local_queue_.Push(current_task_);
        }
        SuspendCurrent();
    }
};
```

### FIFO保证

批处理不影响FIFO顺序：
- Batch内部按LIFO（栈）
- Batch flush到队列保持相对顺序
- Work stealing从队列头部取，保持公平性

---

## 测试策略

### 新增单元测试

**`tests/mpsc_queue_test.cpp`**:
- 单生产者单消费者正确性
- 单生产者多消费者并发
- ABA场景压力测试
- 内存泄漏检测

**`tests/xmm_test.cpp`**:
- 使用XMM的任务验证
- 不使用XMM的任务验证
- 混合场景压力测试

**`tests/worker_batch_test.cpp`**:
- Batch边界测试
- FIFO顺序验证
- Work stealing交互测试

### 回归测试

- 所有现有测试必须100%通过
- 添加多线程压力测试（16-32线程）

### 性能基准

在`benchmark.cpp`中添加：

```cpp
// 高并发Mutex竞争测试
void benchmark_mutex_high_contention(int num_threads);

// XMM保存开销测试
void benchmark_xmm_overhead();

// 多核扩展性详细测试
void benchmark_scalability_detailed();
```

目标性能验证：
- Mutex 16线程: 6M → 15M+ ops/sec
- Yield: 35M → 45M+ yields/sec
- 扩展性: 8核接近8x提升

---

## 实施顺序

### Phase 1: Butex无锁队列（最重要）

1. 修改`TaskMeta`添加`waiter_node`
2. 实现`Butex`的MPSC队列方法
3. 移除`queue_mutex_`
4. 单元测试验证

### Phase 2: WorkStealingQueue padding

1. 添加CACHE_LINE_SIZE常量
2. 添加padding字段
3. 验证cache line对齐

### Phase 3: Worker批处理

1. 添加batch字段
2. 修改`PickTask`逻辑
3. 修改`YieldCurrent`使用batch
4. 单元测试验证

### Phase 4: XMM惰性保存

1. 修改`TaskMeta`添加`uses_xmm`
2. 修改汇编文件添加跳转逻辑
3. 运行时测试验证

---

## 风险与缓解

| 风险 | 缓解措施 |
|------|----------|
| MPSC ABA问题 | `claimed`标记 + 正确内存屏障 |
| XMM保存错误 | 添加运行时断言，失败时打印诊断 |
| FIFO顺序破坏 | 专门的顺序验证测试 |
| Padding失效 | 运行时检测cache line大小 |

---

## 参考资料

- Dmitry Vyukov's MPSC queue: http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
- False sharing: https://en.wikipedia.org/wiki/False_sharing
- Intel optimization manual: XMM寄存器保存开销分析