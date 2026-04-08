# Bthread 代码质量重构实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 全面重构 bthread M:N 线程库，改善目录结构、命名规范、消除代码重复、优化模块划分

**Architecture:** 分 6 阶段执行，每阶段独立验证编译和测试通过后再进入下一阶段。先重组目录，后统一接口，最后拆分大文件。

**Tech Stack:** C++20, CMake 3.15+, Linux/Windows 平台

---

## 文件结构规划

### 头文件改动清单

| 操作 | 原路径 | 新路径 |
|------|--------|--------|
| 移动+重命名 | `include/bthread/task_meta.h` | `include/bthread/core/task_meta.hpp` |
| 移动+重命名 | `include/bthread/butex.h` | `include/bthread/sync/butex.hpp` |
| 移动+重命名 | `include/bthread/global_queue.h` | `include/bthread/queue/global_queue.hpp` |
| 移动+重命名 | `include/bthread/work_stealing_queue.h` | `include/bthread/queue/work_stealing_queue.hpp` |
| 移动+重命名 | `include/bthread/task_group.h` | `include/bthread/core/task_group.hpp` |
| 移动+重命名 | `include/bthread/worker.h` | `include/bthread/core/worker.hpp` |
| 移动+重命名 | `include/bthread/scheduler.h` | 删除（合并到 scheduler.hpp） |
| 移动+重命名 | `include/bthread/once.h` | `include/bthread/api/once.hpp` |
| 移动+重命名 | `include/bthread/timer_thread.h` | `include/bthread/detail/timer_thread.hpp` |
| 移动+重命名 | `include/bthread/execution_queue.h` | `include/bthread/queue/execution_queue.hpp` |
| 新建 | - | `include/bthread/queue/mpsc_queue.hpp` |
| 新建 | - | `include/bthread/sync/waiter.hpp` |
| 新建 | - | `include/bthread/api/spawn.hpp` |
| 新建 | - | `include/bthread/api/config.hpp` |

### 源文件改动清单

| 操作 | 原路径 | 新路径 |
|------|--------|--------|
| 移动 | `src/butex.cpp` | `src/bthread/sync/butex.cpp` |
| 移动 | `src/global_queue.cpp` | `src/bthread/queue/global_queue.cpp` |
| 移动 | `src/work_stealing_queue.cpp` | `src/bthread/queue/work_stealing_queue.cpp` |
| 移动 | `src/task_meta.cpp` | `src/bthread/core/task_meta.cpp` |
| 移动 | `src/task_group.cpp` | `src/bthread/core/task_group.cpp` |
| 移动 | `src/worker.cpp` | `src/bthread/core/worker.cpp` |
| 移动 | `src/scheduler.cpp` | `src/bthread/core/scheduler.cpp` |
| 移动 | `src/once.cpp` | `src/bthread/api/once.cpp` |
| 移动 | `src/timer_thread.cpp` | `src/bthread/detail/timer_thread.cpp` |
| 移动 | `src/execution_queue.cpp` | `src/bthread/queue/execution_queue.cpp` |
| 拆分新建 | - | `src/bthread/sync/butex_queue.cpp` |
| 拆分新建 | - | `src/bthread/core/worker_stealing.cpp` |
| 拆分新建 | - | `src/bthread/core/worker_shutdown.cpp` |

---

## Phase 1: 目录结构重组

### Task 1.1: 创建新目录结构

**Files:**
- Create directories

- [ ] **Step 1: 创建 include 目录**

```bash
mkdir -p include/bthread/queue
mkdir -p include/bthread/api
mkdir -p include/bthread/detail
```

- [ ] **Step 2: 创建 src 目录**

```bash
mkdir -p src/bthread/queue
mkdir -p src/bthread/api
mkdir -p src/bthread/detail
```

- [ ] **Step 3: 验证目录创建**

```bash
ls -la include/bthread/
ls -la src/bthread/
```

Expected: 显示新创建的目录

---

### Task 1.2: 移动 task_meta.h 到 core/task_meta.hpp

**Files:**
- Move: `include/bthread/task_meta.h` → `include/bthread/core/task_meta.hpp`
- Modify: 所有引用该文件的文件

- [ ] **Step 1: 移动文件**

```bash
git mv include/bthread/task_meta.h include/bthread/core/task_meta.hpp
```

- [ ] **Step 2: 更新文件内部 include guard**

Edit `include/bthread/core/task_meta.hpp`:

```cpp
// 修改开头
#pragma once
// 内容保持不变
```

- [ ] **Step 3: 更新引用文件 - bthread.hpp**

Edit `include/bthread.hpp`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 4: 更新引用文件 - api.hpp**

Edit `include/bthread/api.hpp`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 5: 更新引用文件 - butex.h**

Edit `include/bthread/butex.h`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 6: 更新引用文件 - task_group.h**

Edit `include/bthread/task_group.h`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 7: 更新引用文件 - scheduler.h**

Edit `include/bthread/scheduler.h`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 8: 更新引用文件 - worker.h**

Edit `include/bthread/worker.h`:

```cpp
// 旧（如有）:
// #include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 9: 更新 src 文件 - butex.cpp**

Edit `src/butex.cpp`:

```cpp
// 旧:
// #include "bthread/task_meta.h"（如有）
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 10: 更新 src 文件 - task_meta.cpp**

Edit `src/task_meta.cpp`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 11: 更新 src 文件 - worker.cpp**

Edit `src/worker.cpp`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 12: 更新 src 文件 - scheduler.cpp**

Edit `src/scheduler.cpp`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 13: 更新 src 文件 - task_group.cpp**

Edit `src/task_group.cpp`:

```cpp
// 旧:
#include "bthread/task_meta.h"
// 新:
#include "bthread/core/task_meta.hpp"
```

- [ ] **Step 14: 验证编译**

```bash
cd build && make -j$(nproc)
```

Expected: 编译成功，无错误

- [ ] **Step 15: 运行测试验证**

```bash
cd build && ctest --output-on-failure
```

Expected: 所有测试通过

- [ ] **Step 16: 提交**

```bash
git add -A
git commit -m "refactor(phase1): move task_meta.h to core/task_meta.hpp"
```

---

### Task 1.3: 移动 global_queue.h 到 queue/global_queue.hpp

**Files:**
- Move: `include/bthread/global_queue.h` → `include/bthread/queue/global_queue.hpp`
- Move: `src/global_queue.cpp` → `src/bthread/queue/global_queue.cpp`

- [ ] **Step 1: 移动头文件**

```bash
git mv include/bthread/global_queue.h include/bthread/queue/global_queue.hpp
```

- [ ] **Step 2: 移动源文件**

```bash
git mv src/global_queue.cpp src/bthread/queue/global_queue.cpp
```

- [ ] **Step 3: 更新头文件 include**

Edit `include/bthread/queue/global_queue.hpp`:

```cpp
// 内容保持不变，只需确认 #pragma once 存在
#pragma once
```

- [ ] **Step 4: 更新源文件 include**

Edit `src/bthread/queue/global_queue.cpp`:

```cpp
// 旧:
#include "bthread/global_queue.h"
// 新:
#include "bthread/queue/global_queue.hpp"
```

- [ ] **Step 5: 更新引用文件 - scheduler.h**

Edit `include/bthread/scheduler.h`:

```cpp
// 旧:
#include "bthread/global_queue.h"
// 新:
#include "bthread/queue/global_queue.hpp"
```

- [ ] **Step 6: 更新引用文件 - bthread.hpp**

Edit `include/bthread.hpp`（如有引用）:

```cpp
// 添加:
#include "bthread/queue/global_queue.hpp"
```

- [ ] **Step 7: 更新 CMakeLists.txt**

Edit `CMakeLists.txt`:

```cmake
# 旧:
set(BTHREAD_SOURCES
    ...
    src/global_queue.cpp
    ...
)
set(BTHREAD_HEADERS
    ...
    include/bthread/global_queue.h
    ...
)

# 新:
set(BTHREAD_SOURCES
    ...
    src/bthread/queue/global_queue.cpp
    ...
)
set(BTHREAD_HEADERS
    ...
    include/bthread/queue/global_queue.hpp
    ...
)
```

- [ ] **Step 8: 验证编译**

```bash
cd build && make -j$(nproc)
```

Expected: 编译成功

- [ ] **Step 9: 提交**

```bash
git add -A
git commit -m "refactor(phase1): move global_queue to queue directory"
```

---

### Task 1.4: 移动 work_stealing_queue.h 到 queue/work_stealing_queue.hpp

**Files:**
- Move: `include/bthread/work_stealing_queue.h` → `include/bthread/queue/work_stealing_queue.hpp`
- Move: `src/work_stealing_queue.cpp` → `src/bthread/queue/work_stealing_queue.cpp`

- [ ] **Step 1: 移动头文件**

```bash
git mv include/bthread/work_stealing_queue.h include/bthread/queue/work_stealing_queue.hpp
```

- [ ] **Step 2: 移动源文件**

```bash
git mv src/work_stealing_queue.cpp src/bthread/queue/work_stealing_queue.cpp
```

- [ ] **Step 3: 更新源文件 include**

Edit `src/bthread/queue/work_stealing_queue.cpp`:

```cpp
// 旧:
#include "bthread/work_stealing_queue.h"
// 新:
#include "bthread/queue/work_stealing_queue.hpp"
```

- [ ] **Step 4: 更新引用文件 - worker.h**

Edit `include/bthread/worker.h`:

```cpp
// 旧:
#include "bthread/work_stealing_queue.h"
// 新:
#include "bthread/queue/work_stealing_queue.hpp"
```

- [ ] **Step 5: 更新 CMakeLists.txt**

Edit `CMakeLists.txt`:

```cmake
# 修改:
include/bthread/work_stealing_queue.h → include/bthread/queue/work_stealing_queue.hpp
src/work_stealing_queue.cpp → src/bthread/queue/work_stealing_queue.cpp
```

- [ ] **Step 6: 验证编译并提交**

```bash
cd build && make -j$(nproc)
git add -A
git commit -m "refactor(phase1): move work_stealing_queue to queue directory"
```

---

### Task 1.5: 移动 butex.h 到 sync/butex.hpp

**Files:**
- Move: `include/bthread/butex.h` → `include/bthread/sync/butex.hpp`
- Move: `src/butex.cpp` → `src/bthread/sync/butex.cpp`

- [ ] **Step 1: 移动头文件**

```bash
git mv include/bthread/butex.h include/bthread/sync/butex.hpp
```

- [ ] **Step 2: 移动源文件**

```bash
git mv src/butex.cpp src/bthread/sync/butex.cpp
```

- [ ] **Step 3: 更新源文件 include**

Edit `src/bthread/sync/butex.cpp`:

```cpp
// 旧:
#include "bthread/butex.h"
// 新:
#include "bthread/sync/butex.hpp"
```

- [ ] **Step 4: 更新引用文件 - scheduler.h**

Edit `include/bthread/scheduler.h`:

```cpp
// 旧:
#include "bthread/butex.h"
// 新:
#include "bthread/sync/butex.hpp"
```

- [ ] **Step 5: 更新引用文件 - mutex.hpp**

Edit `include/bthread/sync/mutex.hpp`:

```cpp
// 旧:
// #include "bthread/butex.h"（如有）
// 新:
#include "bthread/sync/butex.hpp"
```

- [ ] **Step 6: 更新 mutex.cpp**

Edit `src/bthread/sync/mutex.cpp`:

```cpp
// 旧:
#include "bthread/butex.h"
// 新:
#include "bthread/sync/butex.hpp"
```

- [ ] **Step 7: 更新 CMakeLists.txt**

Edit `CMakeLists.txt`:

```cmake
# 修改:
include/bthread/butex.h → include/bthread/sync/butex.hpp
src/butex.cpp → src/bthread/sync/butex.cpp
```

- [ ] **Step 8: 验证编译并提交**

```bash
cd build && make -j$(nproc)
git add -A
git commit -m "refactor(phase1): move butex to sync directory"
```

---

### Task 1.6: 移动 worker.h 到 core/worker.hpp

**Files:**
- Move: `include/bthread/worker.h` → `include/bthread/core/worker.hpp`
- Move: `src/worker.cpp` → `src/bthread/core/worker.cpp`

- [ ] **Step 1: 移动文件**

```bash
git mv include/bthread/worker.h include/bthread/core/worker.hpp
git mv src/worker.cpp src/bthread/core/worker.cpp
```

- [ ] **Step 2: 更新源文件 include**

Edit `src/bthread/core/worker.cpp`:

```cpp
// 旧:
#include "bthread/worker.h"
// 新:
#include "bthread/core/worker.hpp"
```

- [ ] **Step 3: 更新引用文件 - scheduler.h**

Edit `include/bthread/scheduler.h`:

```cpp
// 旧:
#include "bthread/worker.h"
// 新:
#include "bthread/core/worker.hpp"
```

- [ ] **Step 4: 更新 CMakeLists.txt**

```cmake
# 修改路径
```

- [ ] **Step 5: 验证编译并提交**

```bash
cd build && make -j$(nproc)
git add -A
git commit -m "refactor(phase1): move worker to core directory"
```

---

### Task 1.7: 移动 task_group.h 到 core/task_group.hpp

**Files:**
- Move: `include/bthread/task_group.h` → `include/bthread/core/task_group.hpp`
- Move: `src/task_group.cpp` → `src/bthread/core/task_group.cpp`

- [ ] **Step 1-5: 同上模式，更新所有引用并提交**

```bash
git mv include/bthread/task_group.h include/bthread/core/task_group.hpp
git mv src/task_group.cpp src/bthread/core/task_group.cpp
# 更新 include 引用
# 更新 CMakeLists.txt
cd build && make -j$(nproc)
git add -A
git commit -m "refactor(phase1): move task_group to core directory"
```

---

### Task 1.8: 移动 scheduler.h 合并到 core/scheduler.hpp

**Files:**
- Delete: `include/bthread/scheduler.h`
- Modify: `include/bthread/core/scheduler.hpp`（合并内容）
- Move: `src/scheduler.cpp` → `src/bthread/core/scheduler.cpp`

- [ ] **Step 1: 读取 scheduler.h 内容**

Read `include/bthread/scheduler.h` 的完整内容，准备合并到 scheduler.hpp

- [ ] **Step 2: 合并到 scheduler.hpp**

Edit `include/bthread/core/scheduler.hpp`:

```cpp
// 合并 scheduler.h 的内容到 scheduler.hpp
// 保持 scheduler.hpp 的结构，添加 scheduler.h 中的必要声明
// 确保 #pragma once 存在
```

- [ ] **Step 3: 删除 scheduler.h**

```bash
git rm include/bthread/scheduler.h
```

- [ ] **Step 4: 移动源文件**

```bash
git mv src/scheduler.cpp src/bthread/core/scheduler.cpp
```

- [ ] **Step 5: 更新源文件 include**

Edit `src/bthread/core/scheduler.cpp`:

```cpp
// 旧:
#include "bthread/scheduler.h"
// 新:
#include "bthread/core/scheduler.hpp"
```

- [ ] **Step 6: 更新所有引用 scheduler.h 的文件**

更新:
- `include/bthread/api.hpp`
- `include/bthread.hpp`
- `src/bthread.cpp`
- 其他引用文件

```cpp
// 旧:
#include "bthread/scheduler.h"
// 新:
#include "bthread/core/scheduler.hpp"
```

- [ ] **Step 7: 更新 CMakeLists.txt**

```cmake
# 删除 scheduler.h，修改 scheduler.cpp 路径
```

- [ ] **Step 8: 验证编译并提交**

```bash
cd build && make -j$(nproc)
cd build && ctest --output-on-failure
git add -A
git commit -m "refactor(phase1): merge scheduler.h into core/scheduler.hpp"
```

---

### Task 1.9: 移动 once.h 到 api/once.hpp

**Files:**
- Move: `include/bthread/once.h` → `include/bthread/api/once.hpp`
- Move: `src/once.cpp` → `src/bthread/api/once.cpp`

- [ ] **Step 1-5: 按标准模式执行并提交**

---

### Task 1.10: 移动 timer_thread.h 到 detail/timer_thread.hpp

**Files:**
- Move: `include/bthread/timer_thread.h` → `include/bthread/detail/timer_thread.hpp`
- Move: `src/timer_thread.cpp` → `src/bthread/detail/timer_thread.cpp`

- [ ] **Step 1-5: 按标准模式执行并提交**

---

### Task 1.11: 移动 execution_queue.h 到 queue/execution_queue.hpp

**Files:**
- Move: `include/bthread/execution_queue.h` → `include/bthread/queue/execution_queue.hpp`
- Move: `src/execution_queue.cpp` → `src/bthread/queue/execution_queue.cpp`

- [ ] **Step 1-5: 按标准模式执行并提交**

---

### Task 1.12: Phase 1 最终验证

- [ ] **Step 1: 完整编译**

```bash
cd build && make clean && make -j$(nproc)
```

Expected: 无错误

- [ ] **Step 2: 运行全部测试**

```bash
cd build && ctest --output-on-failure -j$(nproc)
```

Expected: 所有测试通过

- [ ] **Step 3: 验证目录结构**

```bash
tree include/bthread/ -L 2
tree src/bthread/ -L 2
```

Expected: 符合设计文档中的目标结构

- [ ] **Step 4: Phase 1 总结提交**

```bash
git add -A
git commit -m "refactor(phase1): complete directory structure reorganization"
```

---

## Phase 2: 命名规范化

### Task 2.1: 重命名 WaiterNode 为 ButexWaiterNode

**Files:**
- Modify: `include/bthread/core/task_meta.hpp`
- Modify: `include/bthread/sync/butex.hpp`
- Modify: `src/bthread/sync/butex.cpp`

- [ ] **Step 1: 在 task_meta.hpp 中重命名**

Edit `include/bthread/core/task_meta.hpp`:

```cpp
// 旧:
struct WaiterNode {
    std::atomic<WaiterNode*> next{nullptr};
    std::atomic<bool> claimed{false};
};

// 新:
struct ButexWaiterNode {
    std::atomic<ButexWaiterNode*> next{nullptr};
    std::atomic<bool> claimed{false};
};
```

- [ ] **Step 2: 更新 TaskMeta 中的字段名**

Edit `include/bthread/core/task_meta.hpp`:

```cpp
// 旧:
WaiterNode waiter_node;

// 新:
ButexWaiterNode butex_waiter_node;
```

- [ ] **Step 3: 更新 butex.hpp**

Edit `include/bthread/sync/butex.hpp`:

```cpp
// 旧:
std::atomic<WaiterNode*> head_{nullptr};
std::atomic<WaiterNode*> tail_{nullptr};

// 新:
std::atomic<ButexWaiterNode*> head_{nullptr};
std::atomic<ButexWaiterNode*> tail_{nullptr};
```

- [ ] **Step 4: 更新 butex.cpp 所有引用**

Edit `src/bthread/sync/butex.cpp`:

```cpp
// 所有 WaiterNode 替换为 ButexWaiterNode
// 所有 waiter_node 替换为 butex_waiter_node
```

- [ ] **Step 5: 验证编译**

```bash
cd build && make -j$(nproc)
```

- [ ] **Step 6: 提交**

```bash
git add -A
git commit -m "refactor(phase2): rename WaiterNode to ButexWaiterNode"
```

---

### Task 2.2: 重命名 Mutex 中的 WaiterNode 为 MutexWaiterNode

**Files:**
- Modify: `include/bthread/sync/mutex.hpp`
- Modify: `src/bthread/sync/mutex.cpp`

- [ ] **Step 1: 在 mutex.hpp 中重命名**

Edit `include/bthread/sync/mutex.hpp`:

```cpp
// 旧:
struct WaiterNode {
    TaskMetaBase* task;
    WaiterNode* next;
};
WaiterNode* waiter_head_{nullptr};
WaiterNode* waiter_tail_{nullptr};

// 新:
struct MutexWaiterNode {
    TaskMetaBase* task;
    MutexWaiterNode* next;
};
MutexWaiterNode* waiter_head_{nullptr};
MutexWaiterNode* waiter_tail_{nullptr};
```

- [ ] **Step 2: 更新 mutex.cpp**

Edit `src/bthread/sync/mutex.cpp`:

```cpp
// 所有 WaiterNode 替换为 MutexWaiterNode
```

- [ ] **Step 3: 验证编译并提交**

```bash
cd build && make -j$(nproc)
git add -A
git commit -m "refactor(phase2): rename Mutex WaiterNode to MutexWaiterNode"
```

---

### Task 2.3: 保持枚举值命名风格（现有风格已符合惯例）

**Files:**
- No changes needed

**说明**: 现有 TaskState 枚举值使用大写（READY、RUNNING、SUSPENDED、FINISHED），这符合 C++ 常见惯例，无需修改。

- [ ] **Step 1: 验证现有风格一致性**

```bash
grep -r "TaskState::READY\|TaskState::RUNNING\|TaskState::SUSPENDED\|TaskState::FINISHED" src/ include/ | wc -l
```

Expected: 约 50+ 处引用，风格一致

- [ ] **Step 2: 确认无需修改**

现有风格一致且符合惯例，跳过此任务。

---

### Task 2.4: Phase 2 最终验证

- [ ] **Step 1: 完整编译和测试**

```bash
cd build && make clean && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 2: Phase 2 总结提交**

```bash
git add -A
git commit -m "refactor(phase2): complete naming standardization"
```

---

## Phase 3: MPSC 队列统一

### Task 3.1: 创建 mpsc_queue.hpp 模板

**Files:**
- Create: `include/bthread/queue/mpsc_queue.hpp`

- [ ] **Step 1: 编写 MPSC 队列模板**

Write `include/bthread/queue/mpsc_queue.hpp`:

```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace bthread {

/**
 * @brief MPSC (Multi-Producer Single-Consumer) lock-free queue template.
 *
 * Thread Safety:
 * - Push(): Safe to call from multiple producer threads concurrently.
 * - Pop(): Must only be called from a single consumer thread.
 *
 * Uses atomic lock-free stack for head and tail pointer for FIFO ordering.
 */
template<typename T>
class MpscQueue {
public:
    MpscQueue() = default;
    ~MpscQueue() = default;

    // Disable copy and move
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    /**
     * @brief Push item to queue (multiple producers).
     */
    void Push(T* item) {
        item->next.store(nullptr, std::memory_order_relaxed);
        T* prev = head_.exchange(item, std::memory_order_acq_rel);
        if (prev) {
            prev->next.store(item, std::memory_order_release);
        } else {
            // First element - set tail
            tail_.store(item, std::memory_order_release);
        }
    }

    /**
     * @brief Pop item from queue (single consumer).
     * @return Item pointer, or nullptr if empty
     */
    T* Pop() {
        T* t = tail_.load(std::memory_order_acquire);
        if (!t) return nullptr;

        T* next = static_cast<T*>(t->next.load(std::memory_order_acquire));
        if (next) {
            tail_.store(next, std::memory_order_release);
            t->next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Last element, try to claim
        T* expected = t;
        if (head_.compare_exchange_strong(expected, nullptr,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.store(nullptr, std::memory_order_release);
            t->next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Race condition: another thread just pushed
        while (!t->next.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        T* n = static_cast<T*>(t->next.load(std::memory_order_acquire));
        tail_.store(n, std::memory_order_release);
        t->next.store(nullptr, std::memory_order_relaxed);
        return t;
    }

    /**
     * @brief Check if queue is empty.
     */
    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<T*> head_{nullptr};
    std::atomic<T*> tail_{nullptr};
};

// Type aliases
using TaskQueue = MpscQueue<TaskMetaBase>;

} // namespace bthread
```

- [ ] **Step 2: 提交新建文件**

```bash
git add include/bthread/queue/mpsc_queue.hpp
git commit -m "refactor(phase3): add unified MpscQueue template"
```

---

### Task 3.2: 重构 GlobalQueue 使用 MpscQueue

**Files:**
- Modify: `include/bthread/queue/global_queue.hpp`
- Modify: `src/bthread/queue/global_queue.cpp`

- [ ] **Step 1: 简化 global_queue.hpp 为别名**

Edit `include/bthread/queue/global_queue.hpp`:

```cpp
#pragma once

#include "bthread/queue/mpsc_queue.hpp"
#include "bthread/core/task_meta_base.hpp"

namespace bthread {

// GlobalQueue is now just an alias for the unified MPSC queue
using GlobalQueue = TaskQueue;

} // namespace bthread
```

- [ ] **Step 2: 删除 global_queue.cpp（不再需要）**

```bash
git rm src/bthread/queue/global_queue.cpp
```

- [ ] **Step 3: 更新 CMakeLists.txt**

移除 global_queue.cpp 的引用

- [ ] **Step 4: 验证编译**

```bash
cd build && make -j$(nproc)
```

- [ ] **Step 5: 提交**

```bash
git add -A
git commit -m "refactor(phase3): refactor GlobalQueue to use MpscQueue alias"
```

---

### Task 3.3: 删除 coro::CoroutineQueue 使用统一队列

**Files:**
- Modify: `include/coro/meta.h`
- Modify: `src/coro/scheduler.cpp`（如有引用）

- [ ] **Step 1: 移除 CoroutineQueue 定义**

Edit `include/coro/meta.h`:

```cpp
// 删除 CoroutineQueue 类定义（约 50 行）
// 添加引用:
#include "bthread/queue/mpsc_queue.hpp"
```

- [ ] **Step 2: 更新引用点**

搜索并更新所有使用 `coro::CoroutineQueue` 的位置，使用 `bthread::TaskQueue`

- [ ] **Step 3: 验证编译并提交**

```bash
cd build && make -j$(nproc)
git add -A
git commit -m "refactor(phase3): remove coro::CoroutineQueue, use unified MpscQueue"
```

---

### Task 3.4: Phase 3 最终验证

- [ ] **Step 1: 编译和测试**

```bash
cd build && make clean && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 2: Phase 3 总结提交**

```bash
git add -A
git commit -m "refactor(phase3): complete MPSC queue unification"
```

---

## Phase 4: 同步原语拆分

### Task 4.1: 创建 sync/waiter.hpp 统一等待状态定义

**Files:**
- Create: `include/bthread/sync/waiter.hpp`

- [ ] **Step 1: 创建 waiter.hpp**

Write `include/bthread/sync/waiter.hpp`:

```cpp
#pragma once

#include <atomic>
#include <cstdint>

namespace bthread {

/**
 * @brief Unified wait state for synchronization primitives.
 */
struct WaitState {
    std::atomic<bool> is_waiting{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> wakeup{false};
    int64_t deadline_us{0};
    int timer_id{0};
};

} // namespace bthread
```

- [ ] **Step 2: 提交**

```bash
git add include/bthread/sync/waiter.hpp
git commit -m "refactor(phase4): add unified WaitState definition"
```

---

### Task 4.2: 拆分 butex.cpp 队列逻辑

**Files:**
- Create: `include/bthread/sync/butex_queue.hpp`
- Create: `src/bthread/sync/butex_queue.cpp`
- Modify: `include/bthread/sync/butex.hpp`
- Modify: `src/bthread/sync/butex.cpp`

- [ ] **Step 1: 创建 butex_queue.hpp**

Write `include/bthread/sync/butex_queue.hpp`:

```cpp
#pragma once

#include "bthread/core/task_meta.hpp"

namespace bthread {

/**
 * @brief Lock-free wait queue operations for Butex.
 */
class ButexQueueOps {
public:
    static void AddToTail(ButexWaiterNode* head, ButexWaiterNode* tail, TaskMeta* task);
    static void AddToHead(ButexWaiterNode* head, ButexWaiterNode* tail, TaskMeta* task);
    static TaskMeta* PopFromHead(ButexWaiterNode* head, ButexWaiterNode* tail);
    static void RemoveFromWaitQueue(TaskMeta* task);
};

} // namespace bthread
```

- [ ] **Step 2: 实现队列操作**

Write `src/bthread/sync/butex_queue.cpp`（从 butex.cpp 移出队列逻辑）

- [ ] **Step 3: 简化 butex.cpp**

保留 Wait/Wake 接口，队列操作调用 ButexQueueOps

- [ ] **Step 4: 更新 CMakeLists.txt**

添加新文件

- [ ] **Step 5: 验证编译并提交**

---

### Task 4.3: 拆分 mutex.cpp

**Files:**
- Create: `include/bthread/sync/mutex_bthread.hpp`
- Create: `src/bthread/sync/mutex_bthread.cpp`
- Create: `include/bthread/sync/mutex_pthread.hpp`
- Create: `src/bthread/sync/mutex_pthread.cpp`
- Create: `include/bthread/sync/mutex_awaiter.hpp`
- Modify: `include/bthread/sync/mutex.hpp`
- Modify: `src/bthread/sync/mutex.cpp`

- [ ] **Step 1: 创建 mutex_bthread.hpp/cpp**

提取 `lock_bthread` 和相关逻辑

- [ ] **Step 2: 创建 mutex_pthread.hpp/cpp**

提取 `lock_pthread` 和 `unlock_pthread` 逻辑

- [ ] **Step 3: 创建 mutex_awaiter.hpp**

提取 `LockAwaiter` 类定义

- [ ] **Step 4: 简化 mutex.cpp**

只保留公共接口 `lock/unlock/try_lock`

- [ ] **Step 5: 验证编译并提交**

---

### Task 4.4: Phase 4 最终验证

- [ ] **Step 1: 编译和测试（包括 mutex 压力测试）**

```bash
cd build && make clean && make -j$(nproc)
cd build && ctest --output-on-failure -R mutex
```

- [ ] **Step 2: Phase 4 总结提交**

---

## Phase 5: Worker 拆分

### Task 5.1: 拆分 worker.cpp 任务窃取逻辑

**Files:**
- Create: `include/bthread/core/worker_stealing.hpp`
- Create: `src/bthread/core/worker_stealing.cpp`

- [ ] **Step 1: 创建 worker_stealing.hpp**

```cpp
#pragma once

#include "bthread/core/task_meta_base.hpp"

namespace bthread {

class Worker;

/**
 * @brief Task stealing operations for Worker.
 */
class WorkerStealingOps {
public:
    static TaskMetaBase* TrySteal(Worker* thief, int attempts);
};

} // namespace bthread
```

- [ ] **Step 2: 实现窃取逻辑**

从 worker.cpp 的 PickTask 中提取窃取部分

- [ ] **Step 3: 更新 worker.cpp**

调用 WorkerStealingOps

- [ ] **Step 4: 验证编译并提交**

---

### Task 5.2: 拆分 worker.cpp shutdown drain 逻辑

**Files:**
- Create: `include/bthread/core/worker_shutdown.hpp`
- Create: `src/bthread/core/worker_shutdown.cpp`

- [ ] **Step 1-4: 同上模式执行**

---

### Task 5.3: Phase 5 最终验证

- [ ] **Step 1: 编译和 Worker 批量测试**

```bash
cd build && make clean && make -j$(nproc)
cd build && ctest --output-on-failure -R worker
```

- [ ] **Step 2: Phase 5 总结提交**

---

## Phase 6: API 整理

### Task 6.1: 拆分 api.hpp 为 spawn.hpp 和 config.hpp

**Files:**
- Create: `include/bthread/api/spawn.hpp`
- Create: `include/bthread/api/config.hpp`
- Modify: `include/bthread/api.hpp`（简化为入口）

- [ ] **Step 1: 创建 spawn.hpp**

提取 spawn 相关函数模板

- [ ] **Step 2: 创建 config.hpp**

提取 `set_worker_count`、`init`、`shutdown` 等

- [ ] **Step 3: 简化 api.hpp**

只做 include 组合

- [ ] **Step 4: 验证编译并提交**

---

### Task 6.2: 更新主入口 bthread.hpp

**Files:**
- Modify: `include/bthread.hpp`

- [ ] **Step 1: 更新 include 列表**

```cpp
#pragma once

// Core
#include "bthread/core/task_meta_base.hpp"
#include "bthread/core/task.hpp"
#include "bthread/core/scheduler.hpp"

// Queue
#include "bthread/queue/mpsc_queue.hpp"

// Sync
#include "bthread/sync/mutex.hpp"
#include "bthread/sync/cond.hpp"
#include "bthread/sync/event.hpp"

// API
#include "bthread/api/spawn.hpp"
#include "bthread/api/config.hpp"

// Coroutine support
#include "coro/coroutine.h"
#include "coro/meta.h"
```

- [ ] **Step 2: 验证 Demo 程序**

```bash
cd build && make -j$(nproc)
cd build && ./demo/demo
```

- [ ] **Step 3: 提交**

---

### Task 6.3: Phase 6 最终验证

- [ ] **Step 1: 完整编译和测试**

```bash
cd build && make clean && make -j$(nproc)
cd build && ctest --output-on-failure
```

- [ ] **Step 2: 运行 Demo**

```bash
cd build && ./demo/demo
cd build && ./demo/yield_demo
```

- [ ] **Step 3: Phase 6 总结提交**

```bash
git add -A
git commit -m "refactor(phase6): complete API organization"
```

---

## 最终验证

### Task 7.1: 全量测试

- [ ] **Step 1: 清理并重新构建**

```bash
cd build && make clean
cd build && cmake ..
cd build && make -j$(nproc)
```

- [ ] **Step 2: 运行所有测试**

```bash
cd build && ctest --output-on-failure -j$(nproc)
```

Expected: 所有测试通过

- [ ] **Step 3: 验证目录结构**

```bash
tree include/bthread/ -L 2
tree src/bthread/ -L 2
```

Expected: 符合设计文档

---

### Task 7.2: 最终总结提交

- [ ] **Step 1: 创建重构总结**

```bash
git add -A
git commit -m "refactor: complete bthread code quality refactoring

Summary:
- Phase 1: Directory structure reorganization
- Phase 2: Naming standardization  
- Phase 3: MPSC queue unification
- Phase 4: Sync primitives split
- Phase 5: Worker split
- Phase 6: API organization

All tests passing.
"
```

---

## 成功标准

1. 所有测试通过
2. 目录结构清晰：core/sync/queue/api/detail/platform
3. 无代码重复（MPSC 队列统一）
4. 命名规范一致（WaiterNode → ButexWaiterNode/MutexWaiterNode）
5. 文件大小适中（100-150行）
6. Demo 程序正常运行