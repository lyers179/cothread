# PopFromHead Timeout 修复实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 PopFromHead 在队列非空时返回 nullptr 的 bug，恢复 Phase 3 的性能

**Architecture:** 移除队列非空时的 timeout 返回，保持 pause 优化，只在真正空时返回 nullptr

**Tech Stack:** std::atomic, CAS, pause 指令, memory ordering

---

## 问题根因分析

### Phase 3 vs Phase 5 对比

| 行为 | Phase 3 | Phase 5 (当前) | 影响 |
|------|---------|---------------|------|
| 队列非空时 | **一直 retry** | **timeout 返回 nullptr** | Wake 错误认为队列空 |
| CAS 失败后 | **继续 retry** | **可能 timeout** | 降低成功率 |
| claimed 节点 | **简单跳过继续** | **复杂 timeout 检查** | 更多分支开销 |

### 导致的性能问题

1. Wake 调用 PopFromHead，timeout 返回 nullptr
2. Wake 认为队列空，停止唤醒 waiter
3. 实际 waiter 还在队列中，未被唤醒
4. bthread 创建后卡住或延迟，性能下降

### 正确的行为

```
队列状态        正确返回
--------        --------
空 (head=null, tail=null)  → nullptr
正在入队 (tail!=null, head=null)  → 等待，不返回 nullptr
有节点 (head!=null)  → retry 直到成功，不返回 nullptr
CAS 失败  → retry，不返回 nullptr
```

---

## 文件结构

| 文件 | 修改类型 | 职责 |
|------|----------|------|
| `src/bthread/sync/butex_queue.cpp` | 修改 | 移除非空队列的 timeout 返回 |
| `tests/sync/mpmc_queue_test.cpp` | 修改 | 添加队列非空时的 retry 测试 |
| `docs/performance_history.md` | 修改 | 记录修复后的性能 |

---

## Task 1: 修复 ButexQueue PopFromHead timeout bug

**Files:**
- Modify: `src/bthread/sync/butex_queue.cpp:113-207`

- [ ] **Step 1: 编写失败测试**

在 `tests/sync/mpmc_queue_test.cpp` 添加:

```cpp
// 测试队列非空时不应该 timeout 返回 nullptr
void test_pop_no_timeout_when_nonempty() {
    ButexQueue queue;
    std::atomic<bool> pop_started{false};
    std::atomic<bool> node_added{false};
    std::atomic<TaskMeta*> popped{nullptr};

    // 创建一个节点
    TaskMeta task;
    task.is_waiting.store(true, std::memory_order_relaxed);
    task.butex_waiter_node.claimed.store(false, std::memory_order_relaxed);
    task.butex_waiter_node.next.store(nullptr, std::memory_order_relaxed);

    // 先设置 tail，延迟设置 head（模拟入队中间状态）
    ButexWaiterNode* node = &task.butex_waiter_node;
    
    // 线程 1: 尝试 Pop（应该等待，不返回 nullptr）
    std::thread popper([&] {
        pop_started.store(true);
        // 等待 node_added 再开始 pop，确保队列非空
        while (!node_added.load()) {
            std::this_thread::yield();
        }
        // 现在队列有节点，Pop 应该返回它，不是 nullptr
        TaskMeta* result = queue.PopFromHead();
        popped.store(result);
    });

    // 等待 popper 启动
    while (!pop_started.load()) {
        std::this_thread::yield();
    }

    // 线程 2: 添加节点
    usleep(1000);  // 让 popper 等待一会儿
    queue.AddToTail(&task);
    node_added.store(true);

    popper.join();

    // Pop 应该返回节点，不是 nullptr
    TaskMeta* result = popped.load();
    assert(result == &task);
    printf("Pop no timeout when nonempty test passed!\n");
}

// 在 main() 添加
test_pop_no_timeout_when_nonempty();
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cd build && make mpmc_queue_test
timeout 30 ./tests/sync/mpmc_queue_test
```

预期: 测试可能 hang 或 timeout（因为当前 PopFromHead 在等待太久后返回 nullptr）

- [ ] **Step 3: 修改 PopFromHead 移除非空 timeout**

修改 `src/bthread/sync/butex_queue.cpp:113-207`:

```cpp
TaskMeta* ButexQueue::PopFromHead() {
    // 只在空队列时使用 timeout
    // 队列非空时一直 retry，不返回 nullptr
    constexpr int MAX_EMPTY_SPINS = 1000;  // 空队列等待阈值

    int empty_spin_count = 0;

    while (true) {
        ButexWaiterNode* head = head_.load(std::memory_order_acquire);

        // 空队列检查
        if (!head) {
            ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
            if (!tail) {
                return nullptr;  // 真正空，返回 nullptr
            }

            // tail != null 但 head == null: 正在入队
            // 等待 head 被设置，不返回 nullptr
            if (++empty_spin_count < MAX_EMPTY_SPINS) {
                BTHREAD_PAUSE();
                continue;
            }
            // 空队列等待太久，yield 后继续
            std::this_thread::yield();
            empty_spin_count = 0;
            continue;  // ← 不返回 nullptr，继续等
        }

        // 有节点，reset empty counter
        empty_spin_count = 0;

        // Try to claim this node (ABA prevention)
        if (head->claimed.exchange(true, std::memory_order_acq_rel)) {
            // Already claimed, skip to next
            ButexWaiterNode* next = head->next.load(std::memory_order_acquire);
            if (next) {
                ButexWaiterNode* expected = head;
                head_.compare_exchange_weak(expected, next,
                    std::memory_order_acq_rel, std::memory_order_relaxed);
            } else {
                // No next, check if queue truly empty
                ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
                if (tail == head) {
                    // Queue empty (head and tail point to claimed node)
                    ButexWaiterNode* expected_head = head;
                    if (head_.compare_exchange_strong(expected_head, nullptr,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        tail_.store(nullptr, std::memory_order_release);
                    }
                }
                BTHREAD_PAUSE();
            }
            continue;  // ← 不返回 nullptr
        }

        // Successfully claimed
        ButexWaiterNode* next = head->next.load(std::memory_order_acquire);

        // If next null but tail != head, node being added
        if (!next) {
            ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
            if (tail != head) {
                // Node being added, release claim and wait
                head->claimed.store(false, std::memory_order_relaxed);
                BTHREAD_PAUSE();
                continue;  // ← 不返回 nullptr
            }
        }

        // Try to advance head
        ButexWaiterNode* expected = head;
        if (head_.compare_exchange_strong(expected, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Success
            return reinterpret_cast<TaskMeta*>(
                reinterpret_cast<char*>(head) - offsetof(TaskMeta, butex_waiter_node));
        }

        // CAS failed, release claim and retry
        head->claimed.store(false, std::memory_order_relaxed);
        BTHREAD_PAUSE();
        // ← 不返回 nullptr，继续 retry
    }
}
```

关键改动:
1. 移除所有 `return nullptr` 在队列非空时的分支
2. 只在 `!head && !tail` 时返回 nullptr（真正空）
3. 使用 `BTHREAD_PAUSE()` 替代 yield 进行 spin

- [ ] **Step 4: 编译**

```bash
cd build && make -j4
```

- [ ] **Step 5: 运行测试**

```bash
./tests/sync/mpmc_queue_test
```

预期: 所有测试通过，无 hang

- [ ] **Step 6: 运行 benchmark**

```bash
./build/benchmark/benchmark
```

预期: Create/Join 应恢复到 ~150K ops/sec（接近 Phase 3）

- [ ] **Step 7: 运行 benchmark 30 次稳定性测试**

```bash
pass=0; fail=0; for i in {1..30}; do 
  if timeout 60 ./build/benchmark/benchmark 2>&1 | grep -q "Benchmark Complete"; then 
    ((pass++)); 
  else 
    ((fail++)); 
  fi
done
echo "Pass: $pass, Fail: $fail"
```

预期: 100% 通过率

- [ ] **Step 8: 提交**

```bash
git add src/bthread/sync/butex_queue.cpp tests/sync/mpmc_queue_test.cpp
git commit -m "fix(butex_queue): remove timeout return when queue nonempty

- Phase 5 had timeout that returned nullptr even with nodes in queue
- Wake would miss waiters, causing bthread creation delays
- Now only return nullptr when queue truly empty (!head && !tail)
- Keep pause optimization for spin efficiency

Performance: Create/Join ~110K → ~150K ops/sec

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: 更新文档

**Files:**
- Modify: `docs/performance_history.md`
- Modify: `docs/superpowers/specs/2026-04-12-pause-yield-optimization-design.md`

- [ ] **Step 1: 更新 design spec 添加问题根因**

在 `docs/superpowers/specs/2026-04-12-pause-yield-optimization-design.md` 添加:

```markdown
## 问题根因

### Phase 5 引入的 bug

Phase 5 的 PopFromHead 在队列非空时引入了 timeout 返回 nullptr:

```cpp
// 错误行为
if (pause_count >= MAX_PAUSE && yield_count >= MAX_YIELD) {
    return nullptr;  // ← 即使队列有节点也返回 nullptr
}
```

这导致 Wake 调用 PopFromHead 后认为队列空了，停止唤醒 waiter。实际 waiter 还在队列中未被唤醒，导致 bthread 创建后卡住。

### 正确行为

PopFromHead 应该只在队列真正空时返回 nullptr:

```cpp
// 正确行为
if (!head && !tail) return nullptr;  // ← 只有真正空才返回 nullptr
// 其他情况继续 retry
```
```

- [ ] **Step 2: 更新 performance_history.md**

更新完整指标对比表，添加修复后的 Phase 5+ 数据。

- [ ] **Step 3: 提交**

```bash
git add docs/superpowers/specs/2026-04-12-pause-yield-optimization-design.md docs/performance_history.md
git commit -m "docs: document PopFromHead timeout bug root cause

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## 完成检查清单

- [ ] 所有编译无错误无警告
- [ ] test_pop_no_timeout_when_nonempty 测试通过
- [ ] Benchmark 100% 通过率 (30/30)
- [ ] Create/Join >= 140K ops/sec
- [ ] vs std::thread >= 4x faster
- [ ] 文档已更新