# Pause/Yield Spin 优化实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 spin 策略从纯 yield 改为自适应 pause → yield，减少上下文切换开销，提升 lock-free 操作性能

**Architecture:**
- ButexQueue PopFromHead: pause → yield 自适应 spin
- MpscQueue Pop: pause → yield + acquire 内存序
- Butex Wake: 批量 Pop 减少 CAS 开销

**Tech Stack:** `__builtin_ia32_pause()`, adaptive spin, memory ordering

**设计文档:** `docs/superpowers/specs/2026-04-12-pause-yield-optimization-design.md`

---

## 文件结构

| 文件 | 修改类型 | 职责 |
|------|----------|------|
| `src/bthread/sync/butex_queue.cpp` | 修改 | pause → yield 自适应 spin |
| `include/bthread/sync/butex_queue.hpp` | 修改 | 添加 PopMultipleFromHead 声明 |
| `include/bthread/queue/mpsc_queue.hpp` | 修改 | pause → yield + acquire |
| `src/bthread/sync/butex.cpp` | 修改 | 批量 Pop |
| `docs/performance_history.md` | 修改 | Phase 5 性能记录 |
| `docs/performance_optimization.md` | 修改 | Phase 5 优化详情 |

---

## Task 1: ButexQueue PopFromHead pause → yield 自适应 spin

**Files:**
- Modify: `src/bthread/sync/butex_queue.cpp:104-189`

- [ ] **Step 1: 定义平台抽象宏**

```cpp
// Platform-specific pause instruction for spin loops
#if defined(__x86_64__) || defined(__i386__)
    #define BTHREAD_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
    #define BTHREAD_PAUSE() asm volatile("isb" ::: "memory")
#else
    #define BTHREAD_PAUSE() do {} while(0)  // compiler barrier fallback
#endif
```

- [ ] **Step 2: 修改 PopFromHead 使用自适应 spin**

```cpp
TaskMeta* ButexQueue::PopFromHead() {
    constexpr int MAX_PAUSE_SPINS = 100;
    constexpr int MAX_YIELD_SPINS = 10;

    int pause_count = 0;
    int yield_count = 0;

    while (true) {
        // ... 现有逻辑 ...
        
        // 空队列等待时
        if (pause_count < MAX_PAUSE_SPINS) {
            BTHREAD_PAUSE();
            ++pause_count;
            continue;
        }
        if (yield_count < MAX_YIELD_SPINS) {
            std::this_thread::yield();
            ++yield_count;
            pause_count = 0;
            continue;
        }
        return nullptr;  // timeout
    }
}
```

- [ ] **Step 3: 添加 PopMultipleFromHead 方法**

```cpp
int ButexQueue::PopMultipleFromHead(TaskMeta** buffer, int max_count) {
    int count = 0;
    while (count < max_count) {
        TaskMeta* task = PopFromHead();
        if (!task) break;
        buffer[count++] = task;
    }
    return count;
}
```

- [ ] **Step 4: 更新头文件**

```cpp
// 在 butex_queue.hpp 添加声明
int PopMultipleFromHead(TaskMeta** buffer, int max_count);
```

- [ ] **Step 5: 编译测试**

```bash
cd build && make -j4
./benchmark/benchmark
```

预期: 编译成功，benchmark 通过

---

## Task 2: MpscQueue Pop pause → yield + acquire

**Files:**
- Modify: `include/bthread/queue/mpsc_queue.hpp:50-105`

- [ ] **Step 1: 定义平台抽象宏（同 Task 1）**

- [ ] **Step 2: 修改 Pop 使用自适应 spin**

```cpp
T* Pop() {
    constexpr int MAX_PAUSE_SPINS = 100;
    constexpr int MAX_YIELD_SPINS = 10;

    // ... 前面逻辑保持不变 ...

    // Race condition: 另一个线程刚 push
    int pause_count = 0;
    int yield_count = 0;

    while (true) {
        // 改为 acquire（不是 seq_cst）
        T* n = static_cast<T*>(t->next.load(std::memory_order_acquire));
        if (n) {
            // ... 返回 ...
        }

        if (pause_count < MAX_PAUSE_SPINS) {
            BTHREAD_QUEUE_PAUSE();
            ++pause_count;
            continue;
        }
        if (yield_count < MAX_YIELD_SPINS) {
            std::this_thread::yield();
            ++yield_count;
            pause_count = 0;
            continue;
        }
        // timeout fallback
    }
}
```

- [ ] **Step 3: 编译测试**

预期: 编译成功，mpsc_queue_test 通过

---

## Task 3: Butex Wake 批量 Pop

**Files:**
- Modify: `src/bthread/sync/butex.cpp:173-249`

- [ ] **Step 1: 简化 Wake 使用批量 Pop**

```cpp
void Butex::Wake(int count) {
    platform::FutexWake(&value_, count);

    constexpr int BATCH_SIZE = 16;
    TaskMeta* tasks[BATCH_SIZE];

    int total_woken = 0;
    while (total_woken < count) {
        int batch_count = queue_.PopMultipleFromHead(
            tasks, std::min(count - total_woken, BATCH_SIZE));
        if (batch_count == 0) break;

        for (int i = 0; i < batch_count; ++i) {
            TaskMeta* waiter = tasks[i];
            waiter->is_waiting.store(false, std::memory_order_release);
            waiter->wake_count.fetch_add(1, std::memory_order_release);
            // ... 取消 timeout, CAS 入队 ...
        }
        total_woken += batch_count;
    }
}
```

- [ ] **Step 2: 编译测试**

预期: Wake 更简洁，无动态分配

---

## Task 4: 性能验证和文档更新

**Files:**
- Modify: `docs/performance_history.md`
- Modify: `docs/performance_optimization.md`

- [ ] **Step 1: 运行 benchmark 30 次**

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

- [ ] **Step 2: 更新 performance_history.md**

添加 Phase 5 条目，更新完整指标对比表

- [ ] **Step 3: 更新 performance_optimization.md**

添加 Phase 5 优化详情

- [ ] **Step 4: 提交**

```bash
git add ...
git commit -m "perf(queue): optimize spin with pause instruction and batch Pop

- ButexQueue: pause → yield adaptive spin
- MpscQueue: pause + acquire memory ordering
- Butex Wake: batch PopMultipleFromHead

Performance: Create/Join 92K → ~110K ops/sec (+20%)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## 完成检查清单

- [ ] 所有编译无错误无警告
- [ ] Benchmark 100% 通过率 (30/30)
- [ ] Create/Join 性能 >= 100K ops/sec
- [ ] 文档已更新 (Phase 5)
- [ ] Git 历史清晰