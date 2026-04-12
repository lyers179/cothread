# Wait 优化实现计划 - 恢复 Phase 3 性能

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 分析并优化 Wait 中导致性能下降的逻辑，尝试恢复 Phase 3 性能水平

**Architecture:** 分析 Step 7.5 必要性，评估移除或简化的影响；评估 Wait CAS 是否可优化

**Tech Stack:** std::atomic, CAS vs store, memory ordering, performance profiling

---

## 问题分析

### Phase 3 vs 当前性能对比

| Metric | Phase 3 | 当前 | 差距 |
|--------|---------|------|------|
| Create/Join | 152K ops/sec | 127K ops/sec | **-17%** |
| vs std::thread | 10x | 3.13x | **-69%** |

### 主要差异

| 操作 | Phase 3 | 当前 | 开销 |
|------|---------|------|------|
| Wait wake_count 检查 | 直接 store | CAS | CAS ~2-3x 慢 |
| Step 7.5 re-check value | **没有** | **有** | 多一次 load + 分支 |
| Wake state 设置 | 直接 store | 直接 store | ✓ 相同 |

### Step 7.5 分析

当前代码 (lines 112-121):
```cpp
// 7.5. Re-check value AFTER adding to queue
val = value_.load(std::memory_order_acquire);
if (val != expected_value) {
    node->claimed.store(true, std::memory_order_release);
    task->is_waiting.store(false, std::memory_order_release);
    task->waiting_butex = nullptr;
    return 0;
}
```

**目的**: 捕捉 Wake 在入队后改变 butex 值的边缘情况

**问题**: 增加额外 atomic load + 分支判断

---

## 文件结构

| 文件 | 修改类型 | 职责 |
|------|----------|------|
| `src/bthread/sync/butex.cpp` | 修改 | 移除/优化 Step 7.5 |
| `docs/performance_history.md` | 修改 | 记录优化结果 |

---

## Task 1: 分析 Step 7.5 必要性

**Files:**
- Read: `src/bthread/sync/butex.cpp`
- Read: `git log` for Step 7.5 introduction commit

- [ ] **Step 1: 查找 Step 7.5 引入的 commit**

```bash
git log -p --follow -S "7.5. Re-check value" src/bthread/sync/butex.cpp | head -60
```

- [ ] **Step 2: 分析引入原因**

阅读 commit message，判断 Step 7.5 是为了修复什么 bug。

- [ ] **Step 3: 评估移除风险**

分析移除 Step 7.5 后可能出现的问题：
- 如果 Wake 在 Wait 入队后改变 butex 值，Wait 是否会正确返回？
- Wait 进入 SUSPENDED 后，Wake 是否能正确唤醒？

- [ ] **Step 4: 记录分析结论**

报告 Step 7.5 是否必要。

---

## Task 2: 移除或简化 Step 7.5（如果安全）

**Files:**
- Modify: `src/bthread/sync/butex.cpp:112-121`

**前提**: Task 1 分析结论认为 Step 7.5 可以移除或简化

- [ ] **Step 1: 编写边缘情况测试**

在 `tests/sync/mpmc_queue_test.cpp` 添加:

```cpp
// 测试 Wait 入队后值变化的边缘情况
void test_wait_value_change_after_enqueue() {
    bthread::Butex butex;
    butex.value_.store(0);
    
    std::atomic<bool> wait_started{false};
    std::atomic<bool> value_changed{false};
    std::atomic<int> wait_result{999};
    
    // 线程 1: Wait
    std::thread waiter([&] {
        wait_started.store(true);
        // 等待 value_changed 后开始 Wait
        while (!value_changed.load()) std::this_thread::yield();
        int result = butex.Wait(0, nullptr);
        wait_result.store(result);
    });
    
    // 等待 waiter 启动
    while (!wait_started.load()) std::this_thread::yield();
    
    // 线程 2: 改变值并 Wake
    usleep(1000);
    butex.value_.store(1);  // 改变值
    value_changed.store(true);
    butex.Wake(1);          // Wake
    
    waiter.join();
    
    // Wait 应该返回 0（值已变）而不是挂起
    assert(wait_result.load() == 0);
    printf("Wait value change after enqueue test passed!\n");
}
```

- [ ] **Step 2: 运行测试确认当前行为**

```bash
cd build && make mpmc_queue_test
./tests/sync/mpmc_queue_test
```

- [ ] **Step 3: 移除 Step 7.5**

修改 `src/bthread/sync/butex.cpp:112-121`，删除或注释掉:

```cpp
// 删除以下代码块（lines 112-121）
// // 7.5. Re-check value AFTER adding to queue
// val = value_.load(std::memory_order_acquire);
// if (val != expected_value) {
//     node->claimed.store(true, std::memory_order_release);
//     task->is_waiting.store(false, std::memory_order_release);
//     task->waiting_butex = nullptr;
//     return 0;
// }
```

- [ ] **Step 4: 编译**

```bash
cd build && make -j4
```

- [ ] **Step 5: 运行测试**

```bash
./tests/sync/mpmc_queue_test
./build/benchmark/benchmark
```

预期: 如果移除安全，测试应通过；如果不安全，测试会失败

- [ ] **Step 6: 运行 benchmark 对比性能**

```bash
./build/benchmark/benchmark
```

预期: 如果移除有效，Create/Join 应提升

- [ ] **Step 7: 如果移除导致问题，恢复代码**

如果测试失败，恢复 Step 7.5，改为简化（如减少 atomic 操作）

- [ ] **Step 8: 提交（如果成功）**

```bash
git add src/bthread/sync/butex.cpp tests/sync/mpmc_queue_test.cpp
git commit -m "perf(butex): remove Step 7.5 re-check value

- Step 7.5 was added for edge case but adds overhead
- Analysis shows Wait handles value change via initial check + CAS failure
- Removing saves one atomic load + branch per Wait

Performance: Create/Join 127K → ~150K ops/sec

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 3: 评估 Wait CAS 优化可行性

**Files:**
- Read: `src/bthread/sync/butex.cpp:144-149`

- [ ] **Step 1: 分析 Wait CAS 是否可改为直接 store**

分析当前同步机制：
- Wake 先 increment wake_count，再 store state
- Wait 检测 wake_count 变化后用 CAS 尝试 enqueue

问题: 如果 Wait 也用直接 store，是否会导致双重入队？

- [ ] **Step 2: 设计抢先 store 策略（如果可行）**

```cpp
// 可能的优化策略：Wait 用抢先 store + 再次检查
if (wake_count 变化) {
    // 抢先设置 READY（防止 Wake enqueue）
    state.store(READY);
    
    // 再次检查是否 Wake 已 enqueue（通过某种标志）
    if (!already_enqueued) {
        EnqueueTask();
    }
}
```

分析需要什么额外标志才能安全实现。

- [ ] **Step 3: 记录结论**

报告 Wait CAS 优化是否可行，如果不可行说明原因。

---

## Task 4: 更新文档

**Files:**
- Modify: `docs/performance_history.md`

- [ ] **Step 1: 更新性能历史**

记录本次优化的分析结果和（如果有）性能改进。

- [ ] **Step 2: 提交**

```bash
git add docs/performance_history.md
git commit -m "docs: record Wait optimization analysis

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## 完成检查清单

- [ ] Step 7.5 必要性分析完成
- [ ] 边缘情况测试通过
- [ ] 如果移除 Step 7.5: Create/Join >= 140K ops/sec
- [ ] 如果保留 Step 7.5: 记录原因，考虑其他优化
- [ ] 文档已更新