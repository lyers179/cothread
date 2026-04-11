# Lock-Free Queue 优化设计规格

> **设计日期**: 2026-04-11
> **设计者**: Claude

## 问题分析

### 当前问题

| 组件 | 问题 | 影响 |
|------|------|------|
| **Butex::Wake** | MPSC Queue 要求单消费者，多线程 Wake 需要 `wake_mutex_` | 高频 Wake 调用产生锁竞争 |
| **ExecutionQueue** | 使用 `std::queue` + `std::mutex` | 任务提交/执行热路径有锁开销 |

### 症状

1. 高并发场景下 Wake 调用频繁，`wake_mutex_` 成为瓶颈
2. ExecutionQueue 的 Submit/ExecuteOne 都需要获取锁

## 优化目标

1. **Butex::Wake**: 改用 MPMC Queue，消除 `wake_mutex_`
2. **ExecutionQueue**: 改用 Lock-Free 队列，消除 `tasks_mutex_`

## 设计方案

### 方案 A: MPMC Queue（推荐）

将 ButexQueue 从 MPSC 改为 MPMC（多生产者多消费者）。

**核心思路**:
- PopFromHead 改用 CAS 竞争，失败则 retry
- 多个消费者可以并发 Pop，无需 mutex

**关键算法**:
```
PopFromHead:
1. Load head
2. If head == null, return null
3. CAS head -> head.next
4. If CAS succeeds, return head.task
5. If CAS fails (other consumer got it), retry from step 1
```

**内存序**:
- Push: `acq_rel`（生产者之间同步）
- Pop: `acq_rel`（消费者之间同步）
- Load next: `acquire`（看到生产者的链接）

### 方案 B: Per-Worker Buckets（不推荐）

每个 worker 有独立的 bucket，复杂度高，内存开销大。

## 技术细节

### MPMC Queue 实现

```cpp
class MpmcQueue {
    std::atomic<Node*> head_{nullptr};
    std::atomic<Node*> tail_{nullptr};
    
    // Push: 多生产者，使用 CAS
    void Push(TaskMeta* task) {
        Node* node = &task->waiter_node;
        node->next.store(nullptr, relaxed);
        
        Node* prev = tail_.exchange(node, acq_rel);
        if (prev) {
            prev->next.store(node, release);
        } else {
            head_.store(node, release);
        }
    }
    
    // Pop: 多消费者，使用 CAS retry
    TaskMeta* Pop() {
        while (true) {
            Node* head = head_.load(acquire);
            if (!head) return nullptr;
            
            Node* next = head->next.load(acquire);
            
            // CAS 争夺 head
            if (head_.compare_exchange_weak(head, next, acq_rel, acquire)) {
                // 成功获取
                return head->task;
            }
            // CAS 失败，其他消费者已获取，retry
        }
    }
};
```

###ABA 问题防护

使用 `claimed` 标志位防止 ABA：
- Pop 成功后设置 `claimed = true`
- 被 claimed 的节点不会被再次 Pop

### ExecutionQueue 改造

使用已有的 `MpscQueue` 或新的 `MpmcQueue`：
- Submit = Push（多生产者）
- ExecuteOne = Pop（单消费者，MPSC 即可）

## 影响分析

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `include/bthread/sync/butex_queue.hpp` | 改为 MPMC 接口 |
| `src/bthread/sync/butex_queue.cpp` | PopFromHead 改用 CAS retry |
| `include/bthread/sync/butex.hpp` | 移除 `wake_mutex_` |
| `src/bthread/sync/butex.cpp` | Wake 中移除 mutex lock |
| `include/bthread/queue/execution_queue.hpp` | 改用 lock-free queue |
| `src/bthread/queue/execution_queue.cpp` | Submit/ExecuteOne 无锁 |

### 性能预期

| 指标 | 当前 | 预期改进 |
|------|------|----------|
| Wake contention | mutex overhead | 无锁 CAS retry |
| ExecutionQueue latency | mutex acquire/release | 原子操作 |

## 测试计划

1. 单元测试：MPMC Queue 正确性
2. 并发测试：多线程 Wake 正确性
3. 性能测试：Benchmark 对比

## 风险评估

| 风险 | 缓解措施 |
|------|----------|
| ABA 问题 | `claimed` 标志位防护 |
| CAS retry 开销 | 自适应 spin + yield |
| 内存序错误 | 仔细分析每个操作的同步需求 |

## 参考资料

- [MPMC Queue Design](https://www.1024cores.net/home/lock-free-algorithms/queues)
- Dmitry Vyukov's bounded MPMC queue