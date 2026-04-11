# Lock-Free Queue 优化实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Butex Wake 和 ExecutionQueue 从 lock-based 改为 lock-free，减少高并发场景下的锁竞争

**Architecture:** 
- ButexQueue: MPSC → MPMC，PopFromHead 改用 CAS retry 消除 wake_mutex_
- ExecutionQueue: std::queue → MpscQueue，任务提交无锁

**Tech Stack:** std::atomic, CAS, memory ordering (acq_rel/acquire/release)

**设计文档:** `docs/superpowers/specs/2026-04-11-lockfree-queue-optimization-design.md`

---

## 文件结构

| 文件 | 修改类型 | 职责 |
|------|----------|------|
| `include/bthread/sync/butex_queue.hpp` | 修改 | MPMC Queue 接口声明 |
| `src/bthread/sync/butex_queue.cpp` | 修改 | PopFromHead CAS retry 实现 |
| `include/bthread/sync/butex.hpp` | 修改 | 移除 wake_mutex_ |
| `src/bthread/sync/butex.cpp` | 修改 | Wake 中移除 mutex lock |
| `include/bthread/queue/execution_queue.hpp` | 修改 | 使用 MpscQueue |
| `src/bthread/queue/execution_queue.cpp` | 修改 | 无锁 Submit/ExecuteOne |
| `tests/sync/mpmc_queue_test.cpp` | 创建 | MPMC Queue 单元测试 |

---

## Task 1: MPMC Queue PopFromHead 改造

**Files:**
- Modify: `src/bthread/sync/butex_queue.cpp:104-163`
- Modify: `include/bthread/sync/butex_queue.hpp:52`
- Test: 新增测试验证多消费者正确性

- [ ] **Step 1: 编写 MPMC PopFromHead 测试**

创建 `tests/sync/mpmc_queue_test.cpp`:

```cpp
#include "bthread/sync/butex_queue.hpp"
#include "bthread/core/task_meta.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>

// 测试多消费者并发 Pop
void test_mpmc_concurrent_pop() {
    ButexQueue queue;
    std::atomic<int> popped_count{0};
    std::atomic<bool> done{false};
    
    // 创建 100 个 waiter
    std::vector<TaskMeta> tasks(100);
    for (auto& t : tasks) {
        t.is_waiting.store(true, relaxed);
        t.butex_waiter_node.claimed.store(false, relaxed);
        queue.AddToTail(&t);
    }
    
    // 4 个消费者并发 Pop
    std::vector<std::thread> consumers;
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            while (!done.load()) {
                TaskMeta* t = queue.PopFromHead();
                if (t) {
                    popped_count.fetch_add(1);
                }
                if (popped_count.load() >= 100) {
                    done.store(true);
                }
            }
        });
    }
    
    for (auto& c : consumers) c.join();
    
    assert(popped_count.load() == 100);
}

int main() {
    test_mpmc_concurrent_pop();
    printf("MPMC Queue test passed!\n");
    return 0;
}
```

- [ ] **Step 2: 编译并运行测试（预期失败）**

```bash
cd build && make mpmc_queue_test
./tests/mpmc_queue_test
```

预期: 测试可能 hang 或 crash（当前 PopFromHead 不是 MPMC）

- [ ] **Step 3: 修改 PopFromHead 为 CAS retry**

修改 `src/bthread/sync/butex_queue.cpp:104-163`:

```cpp
TaskMeta* ButexQueue::PopFromHead() {
    // MPMC: 多消费者使用 CAS retry
    while (true) {
        ButexWaiterNode* head = head_.load(std::memory_order_acquire);
        if (!head) {
            // 检查是否有节点正在入队
            ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
            if (!tail) {
                return nullptr;  // 空队列
            }
            // tail != null 但 head == null，节点正在入队
            // 等待 link 完成
            std::this_thread::yield();
            continue;
        }
        
        // 检查是否已被 claimed（ABA 防护）
        if (head->claimed.load(std::memory_order_acquire)) {
            // 已被其他消费者获取，尝试跳过
            ButexWaiterNode* next = head->next.load(std::memory_order_acquire);
            if (next) {
                // 尝试推进 head
                ButexWaiterNode* expected = head;
                head_.compare_exchange_weak(expected, next,
                    std::memory_order_acq_rel, std::memory_order_acquire);
            } else {
                // next 为空，可能正在入队，yield 等待
                std::this_thread::yield();
            }
            continue;
        }
        
        // 尝试 CAS 争夺 head
        ButexWaiterNode* next = head->next.load(std::memory_order_acquire);
        
        if (head_.compare_exchange_weak(head, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // CAS 成功，获取了 head
            // 设置 claimed 防止再次被 Pop
            head->claimed.store(true, std::memory_order_release);
            
            // 通过偏移计算 TaskMeta 指针
            return reinterpret_cast<TaskMeta*>(
                reinterpret_cast<char*>(head) - offsetof(TaskMeta, butex_waiter_node));
        }
        
        // CAS 失败，其他消费者已获取，retry
    }
}
```

- [ ] **Step 4: 更新头文件注释**

修改 `include/bthread/sync/butex_queue.hpp:26`:

```cpp
/**
 * @brief Lock-free MPMC queue for butex waiters.
 *
 * This class implements a lock-free multiple-producer multiple-consumer queue
 * for managing waiting tasks in synchronization primitives.
 *
 * The queue supports:
 * - AddToTail: FIFO ordering (multiple producers)
 * - AddToHead: LIFO ordering (for re-queueing)
 * - PopFromHead: MPMC consumer operation (CAS retry)
 *
 * Thread safety:
 * - AddToTail/AddToHead can be called from multiple threads concurrently
 * - PopFromHead can be called from multiple threads concurrently (MPMC)
 */
```

- [ ] **Step 5: 编译并运行测试**

```bash
cd build && make -j4
./tests/mpmc_queue_test
```

预期: `MPMC Queue test passed!`

- [ ] **Step 6: 提交**

```bash
git add src/bthread/sync/butex_queue.cpp include/bthread/sync/butex_queue.hpp tests/sync/mpmc_queue_test.cpp
git commit -m "feat(butex_queue): change PopFromHead to MPMC with CAS retry

- Remove single-consumer restriction
- Multiple threads can now PopFromHead concurrently
- Add claimed flag check for ABA prevention
- Add mpmc_queue_test for concurrent pop validation

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 2: 移除 Butex wake_mutex_

**Files:**
- Modify: `include/bthread/sync/butex.hpp:45`
- Modify: `src/bthread/sync/butex.cpp:190-191`

- [ ] **Step 1: 编写无锁 Wake 测试**

在 `tests/sync/mpmc_queue_test.cpp` 添加:

```cpp
#include "bthread/sync/butex.hpp"
#include "bthread.h"

// 测试多线程并发 Wake
void test_butex_concurrent_wake() {
    bthread::Butex butex;
    std::atomic<int> wait_count{0};
    std::atomic<int> wake_count{0};
    
    // 10 个 bthread 等待
    const int N = 10;
    std::vector<bthread_t> tids(N);
    
    for (int i = 0; i < N; ++i) {
        bthread_create(&tids[i], nullptr, [](void* arg) -> void* {
            auto* butex = static_cast<bthread::Butex*>(arg);
            butex->Wait(0, nullptr);
            return nullptr;
        }, &butex);
    }
    
    // 等待所有 bthread 进入等待
    usleep(10000);
    
    // 4 个线程并发 Wake
    std::vector<std::thread> wakers;
    for (int i = 0; i < 4; ++i) {
        wakers.emplace_back([&] {
            butex.Wake(3);  // 每个 wake 3 个
        });
    }
    
    for (auto& w : wakers) w.join();
    
    for (int i = 0; i < N; ++i) {
        bthread_join(tids[i], nullptr);
    }
    
    printf("Concurrent Wake test passed!\n");
}

// 在 main() 添加
test_butex_concurrent_wake();
```

- [ ] **Step 2: 运行测试（预期通过，但有 mutex overhead）**

```bash
cd build && make mpmc_queue_test
./tests/mpmc_queue_test
```

预期: 通过（Task 1 已让 PopFromHead 支持 MPMC）

- [ ] **Step 3: 移除 wake_mutex_ 成员**

修改 `include/bthread/sync/butex.hpp:42-45`:

```cpp
private:
    ButexQueue queue_;           // Lock-free MPMC queue for waiters
    std::atomic<int> value_{0};  // Current value
    // 移除: std::mutex wake_mutex_;

    // Timeout callback
    static void TimeoutCallback(void* arg);
```

- [ ] **Step 4: 移除 Wake 中的 mutex lock**

修改 `src/bthread/sync/butex.cpp:173-256`:

```cpp
void Butex::Wake(int count) {
    // Wake futex waiters (pthreads)
    platform::FutexWake(&value_, count);

    // 使用静态数组避免动态分配
    constexpr int STATIC_SIZE = 16;
    TaskMeta* static_tasks[STATIC_SIZE];
    std::vector<TaskMeta*> dynamic_tasks;
    TaskMeta** tasks_to_wake = static_tasks;
    int tasks_capacity = STATIC_SIZE;
    int tasks_count = 0;

    // 无锁 PopFromHead（MPMC）
    int woken = 0;
    while (woken < count) {
        TaskMeta* waiter = queue_.PopFromHead();  // 无锁！
        if (!waiter) break;

        waiter->is_waiting.store(false, std::memory_order_release);
        waiter->wake_count.fetch_add(1, std::memory_order_release);

        if (waiter->waiter.timer_id != 0) {
            Scheduler::Instance().GetTimerThread()->Cancel(waiter->waiter.timer_id);
        }

        TaskState state = waiter->state.load(std::memory_order_acquire);
        if (state == TaskState::SUSPENDED) {
            if (tasks_count < tasks_capacity) {
                tasks_to_wake[tasks_count++] = waiter;
            } else {
                try {
                    if (dynamic_tasks.empty()) {
                        dynamic_tasks.reserve(std::max(count, 64));
                        for (int i = 0; i < tasks_count; ++i) {
                            dynamic_tasks.push_back(static_tasks[i]);
                        }
                        tasks_to_wake = dynamic_tasks.data();
                        tasks_capacity = dynamic_tasks.capacity();
                    }
                    dynamic_tasks.push_back(waiter);
                    tasks_count = dynamic_tasks.size();
                    tasks_to_wake = dynamic_tasks.data();
                } catch (...) {
                    waiter->state.store(TaskState::READY, std::memory_order_release);
                    Scheduler::Instance().EnqueueTask(waiter);
                }
            }
        }

        ++woken;
    }

    // 唤醒任务
    for (int i = 0; i < tasks_count; ++i) {
        TaskMeta* waiter = tasks_to_wake[i];
        TaskState expected = TaskState::SUSPENDED;
        if (waiter->state.compare_exchange_strong(expected, TaskState::READY,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            Scheduler::Instance().EnqueueTask(waiter);
        }
    }
}
```

- [ ] **Step 5: 移除 mutex include**

修改 `include/bthread/sync/butex.hpp:5`:

```cpp
#include <atomic>
#include <cstdint>
// 移除: #include <mutex>
#include "bthread/sync/butex_queue.hpp"
```

- [ ] **Step 6: 编译并运行测试**

```bash
cd build && make -j4
./tests/mpmc_queue_test
```

预期: `MPMC Queue test passed!` 和 `Concurrent Wake test passed!`

- [ ] **Step 7: 运行完整 benchmark**

```bash
./build/benchmark/benchmark
```

预期: 所有测试通过，Mutex Contention 时间应减少

- [ ] **Step 8: 提交**

```bash
git add include/bthread/sync/butex.hpp src/bthread/sync/butex.cpp tests/sync/mpmc_queue_test.cpp
git commit -m "feat(butex): remove wake_mutex_ for lock-free Wake

- ButexQueue now supports MPMC (Task 1)
- Wake() calls PopFromHead without mutex
- Concurrent Wake operations no longer contend on mutex
- Add concurrent wake test

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 3: ExecutionQueue 改用 Lock-Free Queue

**Files:**
- Modify: `include/bthread/queue/execution_queue.hpp`
- Modify: `src/bthread/queue/execution_queue.cpp`

- [ ] **Step 1: 编写 ExecutionQueue 无锁测试**

在 `tests/sync/mpmc_queue_test.cpp` 添加:

```cpp
#include "bthread/queue/execution_queue.hpp"

void test_execution_queue_concurrent() {
    bthread::ExecutionQueue queue;
    std::atomic<int> executed{0};
    
    // 10 个线程并发 Submit
    std::vector<std::thread> producers;
    for (int i = 0; i < 10; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < 100; ++j) {
                queue.Submit([&] {
                    executed.fetch_add(1);
                });
            }
        });
    }
    
    // 1 个消费者执行
    while (executed.load() < 1000) {
        queue.ExecuteOne();
    }
    
    for (auto& p : producers) p.join();
    
    assert(executed.load() == 1000);
    printf("ExecutionQueue concurrent test passed!\n");
}

// 在 main() 添加
test_execution_queue_concurrent();
```

- [ ] **Step 2: 运行测试（预期通过，但有 mutex overhead）**

```bash
cd build && make mpmc_queue_test
./tests/mpmc_queue_test
```

- [ ] **Step 3: 修改 ExecutionQueue 使用 MpscQueue**

修改 `include/bthread/queue/execution_queue.hpp`:

```cpp
#pragma once

#include <functional>
#include <atomic>

#include "bthread/queue/mpsc_queue.hpp"
#include "bthread/platform/platform.h"

namespace bthread {

// Execution queue for ordered task execution
// Now uses lock-free MPSC queue internally
class ExecutionQueue {
public:
    using Task = std::function<void()>;

    ExecutionQueue();
    ~ExecutionQueue();

    ExecutionQueue(const ExecutionQueue&) = delete;
    ExecutionQueue& operator=(const ExecutionQueue&) = delete;

    void Stop();
    bool ExecuteOne();
    int Execute();
    void Submit(Task task);

    bool HasPending() const {
        return pending_.load(std::memory_order_acquire);
    }

private:
    // 内部包装结构，因为 MpscQueue 存储指针
    struct TaskWrapper {
        Task task;
        TaskWrapper* next{nullptr};
    };
    
    MpscQueue queue_;  // Lock-free MPSC queue
    std::atomic<bool> stopped_{false};
    std::atomic<bool> pending_{false};
};

} // namespace bthread
```

- [ ] **Step 4: 实现无锁 Submit/ExecuteOne**

修改 `src/bthread/queue/execution_queue.cpp`:

```cpp
#include "bthread/queue/execution_queue.hpp"

namespace bthread {

ExecutionQueue::ExecutionQueue() = default;

ExecutionQueue::~ExecutionQueue() {
    Stop();
}

void ExecutionQueue::Stop() {
    stopped_.store(true, std::memory_order_release);
    Execute();
}

bool ExecutionQueue::ExecuteOne() {
    if (stopped_.load(std::memory_order_acquire)) {
        return false;
    }

    // 从 MpscQueue Pop（无锁）
    void* item = queue_.PopFromHead();
    if (!item) {
        pending_.store(false, std::memory_order_release);
        return false;
    }

    TaskWrapper* wrapper = static_cast<TaskWrapper*>(item);
    Task task = std::move(wrapper->task);
    delete wrapper;

    if (task) {
        task();
        return true;
    }
    return false;
}

int ExecutionQueue::Execute() {
    int executed = 0;
    while (ExecuteOne()) {
        ++executed;
    }
    return executed;
}

void ExecutionQueue::Submit(Task task) {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }

    // 创建 wrapper 并 Push（无锁）
    TaskWrapper* wrapper = new TaskWrapper();
    wrapper->task = std::move(task);
    
    queue_.AddToTail(wrapper);
    pending_.store(true, std::memory_order_release);
}

} // namespace bthread
```

- [ ] **Step 5: 检查 MpscQueue 是否支持 void* 类型**

检查 `include/bthread/queue/mpsc_queue.hpp`:

```bash
grep "class MpscQueue" include/bthread/queue/mpsc_queue.hpp
```

如果 MpscQueue 是模板类或支持 void*，直接使用。否则需要调整。

- [ ] **Step 6: 编译并运行测试**

```bash
cd build && make -j4
./tests/mpmc_queue_test
```

预期: 所有测试通过

- [ ] **Step 7: 提交**

```bash
git add include/bthread/queue/execution_queue.hpp src/bthread/queue/execution_queue.cpp tests/sync/mpmc_queue_test.cpp
git commit -m "feat(execution_queue): use lock-free MpscQueue

- Replace std::queue + mutex with MpscQueue
- Submit() is now lock-free (MPSC push)
- ExecuteOne() is now lock-free (single consumer pop)
- Add concurrent submit test

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Task 4: 性能验证和文档更新

**Files:**
- Modify: `docs/performance_history.md`
- Modify: `docs/performance_optimization.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: 运行 benchmark 30 次统计通过率**

```bash
pass=0; fail=0; for i in {1..30}; do 
  if timeout 60 ./build/benchmark/benchmark 2>&1 | grep -q "Benchmark Complete"; then 
    ((pass++)); 
  else 
    ((fail++)); 
  fi
done
echo "Pass: $pass, Fail: $fail, Rate: $((pass*100/30))%"
```

预期: 100% 通过率，Mutex Contention 时间应减少

- [ ] **Step 2: 对比 Mutex Contention 性能**

```bash
./build/benchmark/benchmark 2>&1 | grep -A5 "Benchmark 3"
```

预期: `Mutex Contention` latency 应 < 0.1 ms（之前 ~2.4 ms）

- [ ] **Step 3: 更新 CHANGELOG.md**

在 `[Unreleased]` 下添加:

```markdown
### Changed
- ButexQueue: MPSC → MPMC, PopFromHead 改用 CAS retry
- Butex::Wake: 移除 wake_mutex_，无锁唤醒
- ExecutionQueue: 改用 MpscQueue，无锁任务提交

### Performance
- Concurrent Wake contention eliminated
- ExecutionQueue Submit latency reduced
```

- [ ] **Step 4: 更新 performance_optimization.md**

在提交记录部分添加:

```markdown
### 第四阶段 (2026-04-11) - Lock-Free Queue 优化
- `butex_queue.cpp` feat: MPMC PopFromHead with CAS retry
- `butex.hpp/cpp` feat: remove wake_mutex_ for lock-free Wake
- `execution_queue.hpp/cpp` feat: use MpscQueue for lock-free submit
```

- [ ] **Step 5: 提交文档更新**

```bash
git add CHANGELOG.md docs/performance_history.md docs/performance_optimization.md
git commit -m "docs: update changelog and performance docs for lock-free optimization

Co-Authored-By: Claude <noreply@anthropic.com>"
```

- [ ] **Step 6: 运行完整测试套件**

```bash
cd build && ctest --timeout 30 --output-on-failure
```

预期: 所有测试通过

---

## 完成检查清单

- [ ] 所有编译无错误无警告
- [ ] 所有单元测试通过
- [ ] Benchmark 100% 通过率
- [ ] Mutex Contention 性能提升
- [ ] 文档已更新
- [ ] Git 历史清晰（每个 Task 一个 commit）