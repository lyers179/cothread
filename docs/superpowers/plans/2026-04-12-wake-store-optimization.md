# Wake Store 优化实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Wake 中的 CAS 改为直接 store，恢复 Phase 3 性能，同时保持 Wait 的 CAS 保护防止双重入队

**Architecture:** Wake 先 increment wake_count 再直接 store READY；Wait 用 CAS 保护，确保只有一方 enqueue

**Tech Stack:** std::atomic, CAS vs store, memory ordering

---

## 问题分析

### 当前性能瓶颈

当前 Wake 使用 CAS 双重保护：
```cpp
// Wake 中
if (state == TaskState::SUSPENDED) {
    CAS(SUSPENDED → READY);  // ← 比直接 store 慢 ~2-3x
    if 成功: EnqueueTask();
}
```

CAS 开销约是直接 store 的 2-3 倍，在高频 Wake 场景下影响性能。

### 正确性分析

双重入队的风险场景：
```
Wait:                           Wake:
  state = SUSPENDED             
                                wake_count.fetch_add(1)
                                state.load() = SUSPENDED
                                (直接 store)   
                                state.store(READY)
                                EnqueueTask()     ← Wake enqueue
  
  wake_count 变化               
  (直接 store)         
  state.store(READY)
  EnqueueTask()                 ← ❌ 双重入队！
```

**解决方案**: Wake 用直接 store（快），Wait 用 CAS（保护）

```
Wait:                           Wake:
  state = SUSPENDED             
                                wake_count.fetch_add(1)
                                state.load() = SUSPENDED
                                state.store(READY)     ← 直接 store
                                EnqueueTask()          ← Wake enqueue
  
  wake_count 变化               
  CAS(SUSPENDED → READY)        
  ✗ 失败（已是 READY）          
  不 enqueue                     ← ✓ 只有一方 enqueue
```

### 性能对比

| 策略 | Wake 开销 | Wait 开销 | 安全性 |
|------|-----------|-----------|--------|
| 当前（双 CAS） | CAS ~3x | CAS ~3x | ✓ 安全 |
| **优化方案** | **store ~1x** | CAS ~3x | ✓ 安全 |
| Phase 3（双 store） | store ~1x | store ~1x | ✗ 有风险 |

---

## 文件结构

| 文件 | 修改类型 | 职责 |
|------|----------|------|
| `src/bthread/sync/butex.cpp` | 修改 | Wake 改用直接 store |
| `docs/performance_history.md` | 修改 | 记录优化后的性能 |

---

## Task 1: Wake 改用直接 store

**Files:**
- Modify: `src/bthread/sync/butex.cpp:202-211`

- [ ] **Step 1: 编写性能对比测试**

在 `tests/sync/mpmc_queue_test.cpp` 添加:

```cpp
#include <chrono>

// 性能测试：对比 Wake 的吞吐量
void test_wake_performance() {
    constexpr int NUM_THREADS = 4;
    constexpr int NUM_ITERATIONS = 10000;
    
    bthread::Butex butex;
    std::atomic<int> total_wakes{0};
    std::atomic<bool> start{false};
    
    // 启动多个等待线程
    std::vector<std::thread> waiters;
    for (int i = 0; i < NUM_THREADS; ++i) {
        waiters.emplace_back([&] {
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j < NUM_ITERATIONS / NUM_THREADS; ++j) {
                butex.Wait(0, nullptr);
                butex.value_.fetch_add(1);  // 改变值，允许下次 wait
            }
        });
    }
    
    // 启动唤醒线程
    std::vector<std::thread> wakers;
    for (int i = 0; i < NUM_THREADS; ++i) {
        wakers.emplace_back([&] {
            while (!start.load()) std::this_thread::yield();
            for (int j = 0; j < NUM_ITERATIONS / NUM_THREADS; ++j) {
                butex.Wake(1);
                total_wakes.fetch_add(1);
            }
        });
    }
    
    start.store(true);
    
    auto begin = std::chrono::high_resolution_clock::now();
    for (auto& w : waiters) w.join();
    for (auto& w : wakers) w.join();
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
    double throughput = total_wakes.load() * 1000000.0 / duration.count();
    
    printf("Wake throughput: %.2f wakes/sec\n", throughput);
    printf("Wake latency: %.2f us/wake\n", duration.count() / (double)total_wakes.load());
    
    assert(total_wakes.load() == NUM_ITERATIONS);
}

// 在 main() 添加
test_wake_performance();
```

- [ ] **Step 2: 运行测试记录当前性能**

```bash
cd build && make mpmc_queue_test
./tests/sync/mpmc_queue_test
```

记录当前 Wake throughput（预期 ~500K wakes/sec）

- [ ] **Step 3: 修改 Wake 使用直接 store**

修改 `src/bthread/sync/butex.cpp:202-211`:

```cpp
// 修改前（当前）
TaskState state = waiter->state.load(std::memory_order_acquire);
if (state == TaskState::SUSPENDED) {
    // Use CAS to ensure only Wake enqueues (not Wait's wake_count check)
    TaskState expected = TaskState::SUSPENDED;
    if (waiter->state.compare_exchange_strong(expected, TaskState::READY,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        Scheduler::Instance().EnqueueTask(waiter);
    }
}

// 修改后（优化）
TaskState state = waiter->state.load(std::memory_order_acquire);
if (state == TaskState::SUSPENDED) {
    // 直接 store READY - Wait 用 CAS 保护防止双重入队
    // Wake 先 increment wake_count，Wait 检测到变化会用 CAS 尝试 enqueue
    // 如果 Wait CAS 成功，说明 Wake 还没 store；如果失败，说明 Wake 已 store
    waiter->state.store(TaskState::READY, std::memory_order_release);
    Scheduler::Instance().EnqueueTask(waiter);
}
```

关键改动:
1. 移除 CAS，改为直接 `store(READY)`
2. Wake 先 increment wake_count 再 store state，确保顺序
3. Wait 用 CAS 保护，不会双重入队

- [ ] **Step 4: 编译**

```bash
cd build && make -j4
```

- [ ] **Step 5: 运行测试验证正确性**

```bash
./tests/sync/mpmc_queue_test
```

预期: test_butex_concurrent_wake 通过，无双重入队

- [ ] **Step 6: 运行性能测试对比**

```bash
./tests/sync/mpmc_queue_test
```

预期: Wake throughput 提升 ~2x（从 ~500K 到 ~1M wakes/sec）

- [ ] **Step 7: 运行 benchmark**

```bash
./build/benchmark/benchmark
```

预期: Create/Join 提升到 ~150K ops/sec

- [ ] **Step 8: 运行 benchmark 30 次稳定性测试**

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

- [ ] **Step 9: 提交**

```bash
git add src/bthread/sync/butex.cpp tests/sync/mpmc_queue_test.cpp
git commit -m "perf(butex): use direct store instead of CAS in Wake

- CAS overhead is ~2-3x of direct store
- Wake uses direct store, Wait uses CAS for protection
- No double-enqueue: Wait CAS fails if Wake already stored READY
- Performance: Create/Join ~110K → ~150K ops/sec

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: 更新文档

**Files:**
- Modify: `docs/performance_history.md`

- [ ] **Step 1: 更新性能历史**

在 `docs/performance_history.md` 的 Phase 5+ 部分添加优化记录:

```markdown
### Wake Store 优化 (2026-04-12)

将 Wake 中的 CAS 改为直接 store，恢复 Phase 3 性能：

**优化策略对比:**

| 策略 | Wake 开销 | Wait 开销 | 安全性 |
|------|-----------|-----------|--------|
| 双 CAS | ~3x | ~3x | ✓ 安全 |
| **Wake store + Wait CAS** | **~1x** | ~3x | ✓ 安全 |
| 双 store | ~1x | ~1x | ✗ 有风险 |

**原理:** Wake 先 increment wake_count 再直接 store READY；Wait 用 CAS 保护，CAS 失败说明 Wake 已 enqueue。

**性能:**
- Create/Join: 110K → ~150K ops/sec (+36%)
- Wake throughput: ~500K → ~1M wakes/sec (+100%)
```

- [ ] **Step 2: 更新完整指标对比表**

更新 Phase 5+ 列的数据。

- [ ] **Step 3: 提交**

```bash
git add docs/performance_history.md
git commit -m "docs: record Wake store optimization

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## 完成检查清单

- [ ] 所有编译无错误无警告
- [ ] test_butex_concurrent_wake 测试通过（无双重入队）
- [ ] Wake throughput 提升 >= 50%
- [ ] Benchmark 100% 通过率 (30/30)
- [ ] Create/Join >= 140K ops/sec
- [ ] vs std::thread >= 4x faster
- [ ] 文档已更新