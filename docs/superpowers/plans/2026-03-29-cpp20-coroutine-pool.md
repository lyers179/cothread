# C++20 Coroutine Pool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a C++20 coroutine-based M:N thread pool for general-purpose task scheduling.

**Architecture:** Layered design with user API (Task<T>/SafeTask<T>) on top, CoroutineScheduler in middle. **Note:** Uses dedicated coroutine worker threads (not shared with bthread workers) for Phase 1 simplicity. Future versions may integrate with bthread's Worker pool.

**Tech Stack:** C++20 coroutines, atomic operations, existing bthread infrastructure.

---

## File Structure

**Headers (create in `include/coro/`):**
- `result.h` - Error class, Result<T> template
- `frame_pool.h` - Coroutine frame memory pool
- `meta.h` - CoroutineMeta struct, CoroutineQueue
- `scheduler.h` - CoroutineScheduler class
- `coroutine.h` - Task<T>, SafeTask<T>, co_spawn, yield, sleep
- `mutex.h` - CoMutex class
- `cond.h` - CoCond class
- `cancel.h` - CancellationToken, CancelSource

**Sources (create in `src/coro/`):**
- `frame_pool.cpp` - FramePool implementation
- `scheduler.cpp` - CoroutineScheduler implementation
- `coroutine.cpp` - Task/SafeTask promise implementations
- `mutex.cpp` - CoMutex implementation
- `cond.cpp` - CoCond implementation
- `cancel.cpp` - Cancellation implementation

**Tests (create in `tests/`):**
- `coroutine_test.cpp` - All coroutine tests

**Modify:**
- `CMakeLists.txt` - Add coro sources, upgrade to C++20

---

## Task 1: Error and Result<T>

**Files:**
- Create: `include/coro/result.h`
- Test: `tests/coroutine_test.cpp`

- [ ] **Step 1: Write failing test for Error class**

```cpp
// tests/coroutine_test.cpp
#include "coro/result.h"
#include <cassert>

void test_error_basic() {
    coro::Error err(1, "test error");
    assert(err.code() == 1);
    assert(err.message() == "test error");
}

int main() {
    test_error_basic();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "coro/result.h not found"

- [ ] **Step 3: Create Error and Result<T> header**

```cpp
// include/coro/result.h
#pragma once

#include <string>
#include <variant>
#include <stdexcept>

namespace coro {

class Error {
public:
    Error(int code, std::string message)
        : code_(code), message_(std::move(message)) {}

    int code() const { return code_; }
    const std::string& message() const { return message_; }

private:
    int code_;
    std::string message_;
};

template<typename T>
class Result {
public:
    // Success case
    Result(T value) : data_(std::move(value)) {}

    // Error case
    Result(Error error) : data_(std::move(error)) {}

    bool is_ok() const { return data_.index() == 0; }
    bool is_err() const { return data_.index() == 1; }

    T& value() {
        if (!is_ok()) {
            throw std::runtime_error("Result contains error");
        }
        return std::get<0>(data_);
    }

    const T& value() const {
        if (!is_ok()) {
            throw std::runtime_error("Result contains error");
        }
        return std::get<0>(data_);
    }

    Error& error() {
        if (!is_err()) {
            throw std::runtime_error("Result contains value");
        }
        return std::get<1>(data_);
    }

    const Error& error() const {
        if (!is_err()) {
            throw std::runtime_error("Result contains value");
        }
        return std::get<1>(data_);
    }

private:
    std::variant<T, Error> data_;
};

// Specialization for void
class Result<void> {
public:
    Result() : has_error_(false) {}
    Result(Error error) : has_error_(true), error_(std::move(error)) {}

    bool is_ok() const { return !has_error_; }
    bool is_err() const { return has_error_; }

    Error& error() {
        if (!has_error_) {
            throw std::runtime_error("Result contains success");
        }
        return error_;
    }

    const Error& error() const {
        if (!has_error_) {
            throw std::runtime_error("Result contains success");
        }
        return error_;
    }

private:
    bool has_error_;
    Error error_{0, ""};
};

} // namespace coro
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd g:/bthread && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS (test runs without assertion failure)

- [ ] **Step 5: Extend test for Result<T>**

```cpp
// Add to tests/coroutine_test.cpp
void test_result_value() {
    coro::Result<int> r(42);
    assert(r.is_ok());
    assert(!r.is_err());
    assert(r.value() == 42);
}

void test_result_error() {
    coro::Result<int> r(coro::Error(1, "failed"));
    assert(!r.is_ok());
    assert(r.is_err());
    assert(r.error().code() == 1);
}

void test_result_void() {
    coro::Result<void> ok;
    assert(ok.is_ok());

    coro::Result<void> err(coro::Error(2, "void error"));
    assert(err.is_err());
}
```

- [ ] **Step 6: Run extended tests**

Run: `cd g:/bthread && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
cd g:/bthread && git add include/coro/result.h tests/coroutine_test.cpp && git commit -m "feat(coro): add Error and Result<T> types"
```

---

## Task 2: FramePool

**Files:**
- Create: `include/coro/frame_pool.h`
- Create: `src/coro/frame_pool.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing test for FramePool**

```cpp
// Add to tests/coroutine_test.cpp
#include "coro/frame_pool.h"

void test_frame_pool_basic() {
    coro::FramePool pool;
    pool.Init(4 * 1024, 16);  // 4KB blocks, 16 initial

    void* block = pool.Allocate(4 * 1024);
    assert(block != nullptr);

    pool.Deallocate(block);

    // Can allocate again after deallocate
    void* block2 = pool.Allocate(4 * 1024);
    assert(block2 != nullptr);
    pool.Deallocate(block2);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "coro/frame_pool.h not found"

- [ ] **Step 3: Create FramePool header**

```cpp
// include/coro/frame_pool.h
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>
#include <mutex>

namespace coro {

class FramePool {
public:
    FramePool() = default;
    ~FramePool();

    // Initialize pool with block_size and initial_count
    void Init(size_t block_size, size_t initial_count);

    // Allocate a block (returns nullptr if size > block_size)
    void* Allocate(size_t size);

    // Deallocate a block
    void Deallocate(void* block);

    // Get current block size
    size_t block_size() const { return block_size_; }

private:
    // Free list node (intrusive)
    struct FreeNode {
        FreeNode* next;
    };

    size_t block_size_{0};
    std::vector<void*> allocated_blocks_;  // All allocated memory
    FreeNode* free_list_{nullptr};
    std::mutex mutex_;
};

} // namespace coro
```

- [ ] **Step 4: Create FramePool implementation**

```cpp
// src/coro/frame_pool.cpp
#include "coro/frame_pool.h"
#include <cstdlib>
#include <cstring>

namespace coro {

FramePool::~FramePool() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (void* block : allocated_blocks_) {
        std::free(block);
    }
}

void FramePool::Init(size_t block_size, size_t initial_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_size_ = block_size;

    for (size_t i = 0; i < initial_count; ++i) {
        void* block = std::malloc(block_size);
        if (block) {
            allocated_blocks_.push_back(block);
            FreeNode* node = static_cast<FreeNode*>(block);
            node->next = free_list_;
            free_list_ = node;
        }
    }
}

void* FramePool::Allocate(size_t size) {
    if (size > block_size_) {
        return nullptr;  // Fall back to heap
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_) {
        FreeNode* node = free_list_;
        free_list_ = node->next;
        return node;
    }

    // Expand pool
    void* block = std::malloc(block_size_);
    if (block) {
        allocated_blocks_.push_back(block);
    }
    return block;
}

void FramePool::Deallocate(void* block) {
    if (!block) return;

    std::lock_guard<std::mutex> lock(mutex_);
    FreeNode* node = static_cast<FreeNode*>(block);
    node->next = free_list_;
    free_list_ = node;
}

} // namespace coro
```

- [ ] **Step 5: Update CMakeLists.txt (upgrade to C++20)**

```cmake
# Add to CMakeLists.txt after BTHREAD_SOURCES
# Note: Upgrade project to C++20 for coroutine support
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORO_SOURCES
    src/coro/frame_pool.cpp
)

# Add coro library
add_library(coro STATIC ${CORO_SOURCES})
target_include_directories(coro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
cd g:/bthread && git add include/coro/frame_pool.h src/coro/frame_pool.cpp CMakeLists.txt tests/coroutine_test.cpp && git commit -m "feat(coro): add FramePool for coroutine frame allocation"
```

---

## Task 3: CoroutineMeta

**Files:**
- Create: `include/coro/meta.h`

- [ ] **Step 1: Create CoroutineMeta header**

```cpp
// include/coro/meta.h
#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>

namespace bthread {
    class Worker;
}

namespace coro {

// Coroutine metadata - manages coroutine lifecycle
struct CoroutineMeta {
    enum State : uint8_t {
        READY,
        RUNNING,
        SUSPENDED,
        FINISHED
    };

    std::coroutine_handle<> handle;
    State state{READY};
    bthread::Worker* owner_worker{nullptr};
    std::atomic<bool> cancel_requested{false};
    void* waiting_sync{nullptr};  // CoMutex/CoCond pointer if waiting

    // Intrusive queue linkage
    CoroutineMeta* next{nullptr};

    // Identification
    uint32_t slot_index{0};
    uint32_t generation{0};
};

// Intrusive coroutine queue (MPSC)
class CoroutineQueue {
public:
    void Push(CoroutineMeta* meta) {
        meta->next = nullptr;
        CoroutineMeta* prev = head_.exchange(meta, std::memory_order_acq_rel);
        if (prev) {
            prev->next = meta;
        } else {
            // First element, need to set tail
            std::atomic_thread_fence(std::memory_order_release);
        }
    }

    CoroutineMeta* Pop() {
        CoroutineMeta* t = tail_.load(std::memory_order_acquire);
        if (!t) return nullptr;

        CoroutineMeta* next = t->next;
        if (next) {
            tail_.store(next, std::memory_order_release);
            t->next = nullptr;
            return t;
        }

        // Last element, try to claim
        CoroutineMeta* expected = t;
        if (head_.compare_exchange_strong(expected, nullptr,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.store(nullptr, std::memory_order_release);
            t->next = nullptr;
            return t;
        }

        // Race condition: another thread just pushed
        // Wait for next pointer to be set
        while (!t->next) {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        CoroutineMeta* n = t->next;
        tail_.store(n, std::memory_order_release);
        t->next = nullptr;
        return t;
    }

    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<CoroutineMeta*> head_{nullptr};
    std::atomic<CoroutineMeta*> tail_{nullptr};
};

} // namespace coro
```

- [ ] **Step 2: Write test for CoroutineQueue**

```cpp
// Add to tests/coroutine_test.cpp
#include "coro/meta.h"

void test_coroutine_queue_basic() {
    coro::CoroutineQueue queue;
    assert(queue.Empty());

    coro::CoroutineMeta meta1;
    queue.Push(&meta1);
    assert(!queue.Empty());

    coro::CoroutineMeta* popped = queue.Pop();
    assert(popped == &meta1);
    assert(queue.Empty());
}

void test_coroutine_queue_multi() {
    coro::CoroutineQueue queue;

    coro::CoroutineMeta meta1, meta2, meta3;
    queue.Push(&meta1);
    queue.Push(&meta2);
    queue.Push(&meta3);

    // Pop in order
    assert(queue.Pop() == &meta1);
    assert(queue.Pop() == &meta2);
    assert(queue.Pop() == &meta3);
    assert(queue.Empty());
}
```

- [ ] **Step 3: Run test**

Run: `cd g:/bthread && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
cd g:/bthread && git add include/coro/meta.h tests/coroutine_test.cpp && git commit -m "feat(coro): add CoroutineMeta and CoroutineQueue"
```

---

## Task 4: Basic Task<T> (Without Scheduler)

**Files:**
- Create: `include/coro/coroutine.h`
- Create: `src/coro/coroutine.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing test for basic Task<T>**

```cpp
// Add to tests/coroutine_test.cpp
#include "coro/coroutine.h"

coro::Task<int> simple_coro() {
    co_return 42;
}

void test_task_basic() {
    auto task = simple_coro();
    // Directly resume (no scheduler yet)
    task.handle().resume();
    int result = task.get();
    assert(result == 42);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "coro/coroutine.h not found"

- [ ] **Step 3: Create Task<T> header**

```cpp
// include/coro/coroutine.h
#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include "coro/result.h"
#include "coro/meta.h"
#include "coro/frame_pool.h"

namespace coro {

// Forward declarations
class CoroutineScheduler;
template<typename T> class SafeTask;

// Task<T> - exception-based coroutine return type
template<typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;

    Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Task(const Task&) = delete;
    ~Task() { if (handle_) handle_.destroy(); }

    Task& operator=(Task&& other) noexcept {
        if (handle_) handle_.destroy();
        handle_ = other.handle_;
        other.handle_ = nullptr;
        return *this;
    }

    // Check if coroutine is done
    bool is_done() const { return handle_ && handle_.done(); }

    // Get handle (for scheduler)
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    // Get result (blocking, throws exception on error)
    T get() {
        if (!handle_) {
            throw std::runtime_error("Task has no handle");
        }
        if (!handle_.done()) {
            throw std::runtime_error("Task not completed");
        }
        return handle_.promise().get_result();
    }

    // Awaiter interface
    bool await_ready() {
        return handle_ && handle_.done();
    }

    void await_suspend(std::coroutine_handle<> awaiting) {
        handle_.promise().set_awaiter(awaiting);
    }

    T await_resume() {
        return get();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

// TaskPromise<T> - promise type for Task<T>
template<typename T>
class TaskPromise {
public:
    TaskPromise() : frame_pool_(nullptr) {}

    // Custom operator new using FramePool
    void* operator new(size_t size) {
        return GetGlobalFramePool().Allocate(size);
    }

    void operator delete(void* ptr) {
        GetGlobalFramePool().Deallocate(ptr);
    }

    Task<T> get_return_object() {
        return Task<T>(std::coroutine_handle<TaskPromise>::from_promise(*this));
    }

    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_value(T value) {
        result_ = std::move(value);
        if (awaiter_) {
            awaiter_.resume();
        }
    }

    void unhandled_exception() {
        exception_ = std::current_exception();
        if (awaiter_) {
            awaiter_.resume();
        }
    }

    T get_result() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        return std::move(result_);
    }

    void set_awaiter(std::coroutine_handle<> h) { awaiter_ = h; }

private:
    T result_;
    std::exception_ptr exception_;
    std::coroutine_handle<> awaiter_;
    FramePool* frame_pool_;

    static FramePool& GetGlobalFramePool() {
        static FramePool pool;
        static std::once_flag init_flag;
        std::call_once(init_flag, [] {
            pool.Init(8 * 1024, 32);  // 8KB blocks, 32 initial
        });
        return pool;
    }
};

// Specialization for void
template<>
class Task<void> {
public:
    using promise_type = TaskPromise<void>;

    Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Task(const Task&) = delete;
    ~Task() { if (handle_) handle_.destroy(); }

    bool is_done() const { return handle_ && handle_.done(); }
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    void get() {
        if (!handle_ || !handle_.done()) {
            throw std::runtime_error("Task not completed");
        }
        handle_.promise().get_result();
    }

    bool await_ready() { return handle_ && handle_.done(); }
    void await_suspend(std::coroutine_handle<> awaiting) {
        handle_.promise().set_awaiter(awaiting);
    }
    void await_resume() { get(); }

private:
    std::coroutine_handle<promise_type> handle_;
};

template<>
class TaskPromise<void> {
public:
    void* operator new(size_t size) {
        return GetGlobalFramePool().Allocate(size);
    }

    void operator delete(void* ptr) {
        GetGlobalFramePool().Deallocate(ptr);
    }

    Task<void> get_return_object() {
        return Task<void>(std::coroutine_handle<TaskPromise>::from_promise(*this));
    }

    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() {
        if (awaiter_) awaiter_.resume();
    }

    void unhandled_exception() {
        exception_ = std::current_exception();
        if (awaiter_) awaiter_.resume();
    }

    void get_result() {
        if (exception_) std::rethrow_exception(exception_);
    }

    void set_awaiter(std::coroutine_handle<> h) { awaiter_ = h; }

private:
    std::exception_ptr exception_;
    std::coroutine_handle<> awaiter_;

    static FramePool& GetGlobalFramePool() {
        static FramePool pool;
        static std::once_flag init_flag;
        std::call_once(init_flag, [] {
            pool.Init(8 * 1024, 32);
        });
        return pool;
    }
};

} // namespace coro
```

- [ ] **Step 4: Update CMakeLists.txt**

```cmake
# Add to CORO_SOURCES
set(CORO_SOURCES
    src/coro/frame_pool.cpp
    src/coro/coroutine.cpp
)
```

- [ ] **Step 5: Create empty coroutine.cpp (definitions if needed)**

```cpp
// src/coro/coroutine.cpp
#include "coro/coroutine.h"

// Most implementation is in headers (templates)
// This file is for any non-template definitions

namespace coro {

// Placeholder for future non-template code

} // namespace coro
```

- [ ] **Step 6: Run test**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
cd g:/bthread && git add include/coro/coroutine.h src/coro/coroutine.cpp CMakeLists.txt tests/coroutine_test.cpp && git commit -m "feat(coro): add basic Task<T> with exception handling"
```

---

## Task 5: CoroutineScheduler

**Files:**
- Create: `include/coro/scheduler.h`
- Create: `src/coro/scheduler.cpp`
- Modify: `CMakeLists.txt`

**Design Note:** This implementation uses dedicated coroutine worker threads rather than integrating with bthread's Worker pool. This simplifies Phase 1 and avoids complex coordination between bthread tasks and coroutines. The dedicated workers each run a loop that pops coroutines from a shared global queue.

- [ ] **Step 1: Write failing test for scheduler spawn**

```cpp
// Add to tests/coroutine_test.cpp
#include "coro/scheduler.h"

coro::Task<int> spawn_test_coro() {
    co_return 100;
}

void test_scheduler_spawn_and_wait() {
    coro::CoroutineScheduler::Instance().Init();

    auto task = coro::co_spawn(spawn_test_coro());

    // Wait for completion (polling)
    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 100);

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "coro/scheduler.h not found"

- [ ] **Step 3: Create scheduler header**

```cpp
// include/coro/scheduler.h
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include "coro/meta.h"
#include "coro/coroutine.h"

namespace coro {

// Thread-local current coroutine meta (for yield/suspend operations)
extern thread_local CoroutineMeta* current_coro_meta_;

// Get current coroutine's meta (returns nullptr if not in coroutine)
inline CoroutineMeta* current_coro_meta() { return current_coro_meta_; }

class CoroutineScheduler {
public:
    static CoroutineScheduler& Instance();

    void Init();
    void Shutdown();

    bool running() const {
        return running_.load(std::memory_order_acquire);
    }

    CoroutineQueue& global_queue() { return global_queue_; }

    template<typename T>
    Task<T> Spawn(Task<T> task) {
        CoroutineMeta* meta = AllocMeta();
        meta->handle = task.handle();
        meta->state = CoroutineMeta::READY;

        // Store CoroutineMeta in promise
        task.handle().promise().set_meta(meta);

        EnqueueCoroutine(meta);
        return std::move(task);
    }

    void EnqueueCoroutine(CoroutineMeta* meta);
    CoroutineMeta* AllocMeta();
    void FreeMeta(CoroutineMeta* meta);

    // Get worker count
    size_t worker_count() const { return workers_.size(); }

private:
    CoroutineScheduler() = default;
    ~CoroutineScheduler();

    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::once_flag init_once_;

    std::vector<std::thread> workers_;
    CoroutineQueue global_queue_;

    std::vector<std::unique_ptr<CoroutineMeta>> meta_pool_;
    std::mutex meta_mutex_;

    void InitMetaPool(size_t count);
    void StartCoroutineWorkers(int count);
    void CoroutineWorkerLoop();
};

// co_spawn function
template<typename T>
Task<T> co_spawn(Task<T> task) {
    CoroutineScheduler::Instance().Spawn(std::move(task));
    return std::move(task);
}

// co_spawn_detached - fire and forget
template<typename T>
void co_spawn_detached(Task<T> task) {
    CoroutineScheduler::Instance().Spawn(std::move(task));
    // Task runs without caller waiting
}

} // namespace coro
```

- [ ] **Step 4: Create scheduler implementation**

```cpp
// src/coro/scheduler.cpp
#include "coro/scheduler.h"
#include <thread>
#include <chrono>

namespace coro {

thread_local CoroutineMeta* current_coro_meta_ = nullptr;

CoroutineScheduler& CoroutineScheduler::Instance() {
    static CoroutineScheduler instance;
    return instance;
}

CoroutineScheduler::~CoroutineScheduler() {
    Shutdown();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void CoroutineScheduler::Init() {
    std::call_once(init_once_, [this] {
        InitMetaPool(256);
        running_.store(true, std::memory_order_release);
        StartCoroutineWorkers(4);  // 4 coroutine workers by default
        initialized_.store(true, std::memory_order_release);
    });
}

void CoroutineScheduler::Shutdown() {
    running_.store(false, std::memory_order_release);
}

void CoroutineScheduler::InitMetaPool(size_t count) {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    for (size_t i = 0; i < count; ++i) {
        meta_pool_.push_back(std::make_unique<CoroutineMeta>());
    }
}

CoroutineMeta* CoroutineScheduler::AllocMeta() {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    for (auto& meta : meta_pool_) {
        if (meta->state == CoroutineMeta::FINISHED ||
            meta->handle == nullptr) {
            // Reset meta
            meta->state = CoroutineMeta::READY;
            meta->cancel_requested.store(false, std::memory_order_relaxed);
            meta->waiting_sync = nullptr;
            meta->next = nullptr;
            meta->owner_worker = nullptr;
            return meta.get();
        }
    }
    // Expand pool
    auto meta = std::make_unique<CoroutineMeta>();
    CoroutineMeta* ptr = meta.get();
    meta_pool_.push_back(std::move(meta));
    return ptr;
}

void CoroutineScheduler::FreeMeta(CoroutineMeta* meta) {
    meta->state = CoroutineMeta::FINISHED;
    meta->handle = nullptr;
}

void CoroutineScheduler::EnqueueCoroutine(CoroutineMeta* meta) {
    global_queue_.Push(meta);
}

void CoroutineScheduler::StartCoroutineWorkers(int count) {
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back([this] {
            CoroutineWorkerLoop();
        });
    }
}

void CoroutineScheduler::CoroutineWorkerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        CoroutineMeta* meta = global_queue_.Pop();
        if (!meta) {
            // No work, wait briefly
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // Set current coroutine (for yield operations)
        current_coro_meta_ = meta;
        meta->state = CoroutineMeta::RUNNING;

        // Resume coroutine
        meta->handle.resume();

        // Clear current coroutine
        current_coro_meta_ = nullptr;

        // Handle post-resume state
        if (meta->state == CoroutineMeta::FINISHED) {
            FreeMeta(meta);
        }
    }
}

} // namespace coro
```

- [ ] **Step 5: Update CMakeLists.txt**

```cmake
# Add to CORO_SOURCES
set(CORO_SOURCES
    src/coro/frame_pool.cpp
    src/coro/coroutine.cpp
    src/coro/scheduler.cpp
)
```

- [ ] **Step 6: Extend TaskPromise to hold CoroutineMeta**

```cpp
// Add to TaskPromise in include/coro/coroutine.h
private:
    CoroutineMeta* meta_{nullptr};

public:
    void set_meta(CoroutineMeta* m) { meta_ = m; }
    CoroutineMeta* meta() const { return meta_; }
```

- [ ] **Step 7: Run test**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
cd g:/bthread && git add include/coro/scheduler.h src/coro/scheduler.cpp include/coro/coroutine.h CMakeLists.txt tests/coroutine_test.cpp && git commit -m "feat(coro): add CoroutineScheduler with dedicated workers"
```

---

## Task 6: CoMutex

**Files:**
- Create: `include/coro/mutex.h`
- Create: `src/coro/mutex.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing test for CoMutex**

```cpp
// Add to tests/coroutine_test.cpp
#include "coro/mutex.h"

coro::Task<int> mutex_test_coro(coro::CoMutex& m, int& counter) {
    co_await m.lock();
    counter++;
    m.unlock();
    co_return counter;
}

void test_comutex_basic() {
    coro::CoroutineScheduler::Instance().Init();

    coro::CoMutex mutex;
    int counter = 0;

    auto t1 = coro::co_spawn(mutex_test_coro(mutex, counter));
    auto t2 = coro::co_spawn(mutex_test_coro(mutex, counter));

    while (!t1.is_done() || !t2.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(counter == 2);

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "coro/mutex.h not found"

- [ ] **Step 3: Create CoMutex header**

```cpp
// include/coro/mutex.h
#pragma once

#include <atomic>
#include <coroutine>
#include "coro/meta.h"

namespace coro {

class CoMutex {
public:
    CoMutex();
    ~CoMutex();

    // Lock awaitable
    class LockAwaiter {
    public:
        LockAwaiter(CoMutex& mutex) : mutex_(mutex) {}

        bool await_ready() {
            return mutex_.try_lock();
        }

        bool await_suspend(std::coroutine_handle<> h);
        void await_resume() {}

    private:
        CoMutex& mutex_;
        std::coroutine_handle<> handle_;
    };

    LockAwaiter lock() { return LockAwaiter(*this); }

    bool try_lock();
    void unlock();

private:
    static constexpr uint32_t LOCKED = 1;
    static constexpr uint32_t HAS_WAITERS = 2;

    std::atomic<uint32_t> state_{0};
    CoroutineQueue waiters_;
};

} // namespace coro
```

- [ ] **Step 4: Create CoMutex implementation**

```cpp
// src/coro/mutex.cpp
#include "coro/mutex.h"
#include "coro/scheduler.h"

namespace coro {

CoMutex::CoMutex() = default;
CoMutex::~CoMutex() = default;

bool CoMutex::try_lock() {
    uint32_t expected = 0;
    return state_.compare_exchange_strong(expected, LOCKED,
        std::memory_order_acquire, std::memory_order_relaxed);
}

bool CoMutex::LockAwaiter::await_suspend(std::coroutine_handle<> h) {
    handle_ = h;

    // Try to acquire lock
    if (mutex_.try_lock()) {
        return false;  // Don't suspend, we got the lock
    }

    // Mark that we have waiters
    uint32_t state = mutex_.state_.load(std::memory_order_acquire);
    if ((state & HAS_WAITERS) == 0) {
        mutex_.state_.fetch_or(HAS_WAITERS, std::memory_order_release);
    }

    // Add to waiters queue
    // Need CoroutineMeta for this coroutine
    CoroutineMeta* meta = CoroutineScheduler::Instance().AllocMeta();
    meta->handle = h;
    meta->state = CoroutineMeta::SUSPENDED;
    meta->waiting_sync = &mutex_;

    mutex_.waiters_.Push(meta);

    return true;  // Suspend
}

void CoMutex::unlock() {
    // Clear locked flag
    uint32_t state = state_.load(std::memory_order_acquire);

    // First, clear LOCKED
    state_.fetch_and(~LOCKED, std::memory_order_release);

    // Check if there are waiters
    CoroutineMeta* waiter = waiters_.Pop();
    if (waiter) {
        // Wake the waiter - set state to LOCKED and enqueue
        waiter->state = CoroutineMeta::READY;
        waiter->waiting_sync = nullptr;

        state_.fetch_or(LOCKED, std::memory_order_release);

        CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
    } else {
        // No waiters, clear HAS_WAITERS flag
        state_.fetch_and(~HAS_WAITERS, std::memory_order_release);
    }
}

} // namespace coro
```

- [ ] **Step 5: Update CMakeLists.txt**

```cmake
set(CORO_SOURCES
    src/coro/frame_pool.cpp
    src/coro/coroutine.cpp
    src/coro/scheduler.cpp
    src/coro/mutex.cpp
)
```

- [ ] **Step 6: Run test**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
cd g:/bthread && git add include/coro/mutex.h src/coro/mutex.cpp CMakeLists.txt tests/coroutine_test.cpp && git commit -m "feat(coro): add CoMutex coroutine synchronization"
```

---

## Task 7: CoCond

**Files:**
- Create: `include/coro/cond.h`
- Create: `src/coro/cond.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing test for CoCond**

```cpp
// Add to tests/coroutine_test.cpp
#include "coro/cond.h"

coro::Task<void> producer(coro::CoMutex& m, coro::CoCond& c, int& data) {
    co_await m.lock();
    data = 42;
    c.signal();
    m.unlock();
}

coro::Task<void> consumer(coro::CoMutex& m, coro::CoCond& c, int& data) {
    co_await m.lock();
    while (data == 0) {
        co_await c.wait(m);
    }
    assert(data == 42);
    m.unlock();
}

void test_cocond_basic() {
    coro::CoroutineScheduler::Instance().Init();

    coro::CoMutex mutex;
    coro::CoCond cond;
    int data = 0;

    auto c = coro::co_spawn(consumer(mutex, cond, data));
    auto p = coro::co_spawn(producer(mutex, cond, data));

    while (!c.is_done() || !p.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "coro/cond.h not found"

- [ ] **Step 3: Create CoCond header**

```cpp
// include/coro/cond.h
#pragma once

#include <coroutine>
#include "coro/meta.h"
#include "coro/mutex.h"

namespace coro {

class CoCond {
public:
    CoCond();
    ~CoCond();

    class WaitAwaiter {
    public:
        WaitAwaiter(CoCond& cond, CoMutex& mutex)
            : cond_(cond), mutex_(mutex) {}

        bool await_ready() { return false; }

        bool await_suspend(std::coroutine_handle<> h);
        void await_resume();  // Re-acquires mutex

    private:
        CoCond& cond_;
        CoMutex& mutex_;
        CoroutineMeta* meta_{nullptr};
    };

    WaitAwaiter wait(CoMutex& mutex) { return WaitAwaiter(*this, mutex); }

    void signal();
    void broadcast();

private:
    CoroutineQueue waiters_;
};

} // namespace coro
```

- [ ] **Step 4: Create CoCond implementation**

```cpp
// src/coro/cond.cpp
#include "coro/cond.h"
#include "coro/scheduler.h"

namespace coro {

CoCond::CoCond() = default;
CoCond::~CoCond() = default;

bool CoCond::WaitAwaiter::await_suspend(std::coroutine_handle<> h) {
    // Get or create CoroutineMeta
    meta_ = CoroutineScheduler::Instance().AllocMeta();
    meta_->handle = h;
    meta_->state = CoroutineMeta::SUSPENDED;
    meta_->waiting_sync = &cond_;

    // Unlock mutex before waiting
    mutex_.unlock();

    // Add to waiters queue
    cond_.waiters_.Push(meta_);

    return true;  // Suspend
}

void CoCond::WaitAwaiter::await_resume() {
    // Called when coroutine resumes - need to re-acquire mutex
    // Use a blocking lock since we're in the coroutine context
    // This is safe because the mutex is not held by anyone else at this point
    while (!mutex_.try_lock()) {
        // If try_lock fails, another coroutine has the lock
        // We need to wait for it properly using co_await
        // For simplicity, we use a spin-yield approach here
        // A more sophisticated implementation would use the scheduler
        std::this_thread::yield();
    }
    // Mutex is now held by us
}

void CoCond::signal() {
    CoroutineMeta* waiter = waiters_.Pop();
    if (waiter) {
        waiter->state = CoroutineMeta::READY;
        waiter->waiting_sync = nullptr;
        CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
    }
}

void CoCond::broadcast() {
    while (!waiters_.Empty()) {
        CoroutineMeta* waiter = waiters_.Pop();
        if (waiter) {
            waiter->state = CoroutineMeta::READY;
            waiter->waiting_sync = nullptr;
            CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
        }
    }
}

} // namespace coro
```

- [ ] **Step 5: Update CMakeLists.txt**

```cmake
set(CORO_SOURCES
    src/coro/frame_pool.cpp
    src/coro/coroutine.cpp
    src/coro/scheduler.cpp
    src/coro/mutex.cpp
    src/coro/cond.cpp
)
```

- [ ] **Step 6: Run test**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
cd g:/bthread && git add include/coro/cond.h src/coro/cond.cpp CMakeLists.txt tests/coroutine_test.cpp && git commit -m "feat(coro): add CoCond coroutine condition variable"
```

---

## Task 8: Cancellation

**Files:**
- Create: `include/coro/cancel.h`
- Create: `src/coro/cancel.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing test for cancellation**

```cpp
// Add to tests/coroutine_test.cpp
#include "coro/cancel.h"

coro::Task<int> cancelable_coro(coro::CancellationToken& token) {
    int count = 0;
    while (count < 100) {
        bool cancelled = co_await token.check_cancel();
        if (cancelled) {
            co_return -1;  // Cancelled
        }
        count++;
        co_await coro::yield();
    }
    co_return count;
}

void test_cancellation() {
    coro::CoroutineScheduler::Instance().Init();

    coro::CancelSource source;
    auto task = coro::co_spawn(cancelable_coro(source.token()));

    // Let it run a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Request cancel
    source.cancel();

    // Wait for completion
    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == -1);  // Was cancelled

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "coro/cancel.h not found"

- [ ] **Step 3: Create cancel header**

```cpp
// include/coro/cancel.h
#pragma once

#include <atomic>
#include <coroutine>
#include "coro/meta.h"

namespace coro {

class CancellationToken {
public:
    CancellationToken(std::atomic<bool>& flag) : flag_(&flag) {}

    bool is_cancelled() const {
        return flag_->load(std::memory_order_acquire);
    }

    class CheckCancelAwaiter {
    public:
        CheckCancelAwaiter(std::atomic<bool>& flag) : flag_(flag) {}

        bool await_ready() { return true; }
        void await_suspend(std::coroutine_handle<>) {}
        bool await_resume() {
            return flag_.load(std::memory_order_acquire);
        }

    private:
        std::atomic<bool>& flag_;
    };

    CheckCancelAwaiter check_cancel() {
        return CheckCancelAwaiter(*flag_);
    }

private:
    std::atomic<bool>* flag_;
};

class CancelSource {
public:
    CancelSource() : cancelled_(false) {}

    CancellationToken token() {
        return CancellationToken(cancelled_);
    }

    void cancel() {
        cancelled_.store(true, std::memory_order_release);
    }

    bool is_cancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> cancelled_;
};

} // namespace coro
```

- [ ] **Step 4: Create cancel implementation (minimal)**

```cpp
// src/coro/cancel.cpp
#include "coro/cancel.h"

namespace coro {

// Most implementation is inline in header

} // namespace coro
```

- [ ] **Step 5: Add yield() function**

```cpp
// Add to include/coro/coroutine.h

// Yield awaiter - suspends current coroutine and re-queues it
class YieldAwaiter {
public:
    bool await_ready() { return false; }  // Always suspend

    bool await_suspend(std::coroutine_handle<> h) {
        // Get CoroutineMeta from thread-local (set by CoroutineWorkerLoop)
        CoroutineMeta* meta = current_coro_meta();
        if (meta) {
            meta->state = CoroutineMeta::READY;
            CoroutineScheduler::Instance().EnqueueCoroutine(meta);
        } else {
            // Not in scheduler context, just resume immediately
            return false;
        }
        return true;  // Suspend
    }

    void await_resume() {}
};

// Yield function - allows other coroutines to run
inline YieldAwaiter yield() { return YieldAwaiter{}; }
```

- [ ] **Step 6: Update CMakeLists.txt**

```cmake
set(CORO_SOURCES
    src/coro/frame_pool.cpp
    src/coro/coroutine.cpp
    src/coro/scheduler.cpp
    src/coro/mutex.cpp
    src/coro/cond.cpp
    src/coro/cancel.cpp
)
```

- [ ] **Step 7: Run test**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
cd g:/bthread && git add include/coro/cancel.h src/coro/cancel.cpp include/coro/coroutine.h CMakeLists.txt tests/coroutine_test.cpp && git commit -m "feat(coro): add cancellation support and yield()"
```

---

## Task 9: SafeTask<T>

**Files:**
- Modify: `include/coro/coroutine.h`

- [ ] **Step 1: Write failing test for SafeTask**

```cpp
// Add to tests/coroutine_test.cpp
coro::SafeTask<int> safe_coro() {
    co_return 100;
}

coro::SafeTask<int> safe_error_coro() {
    co_return coro::Error(5, "test error");
}

void test_safetask_basic() {
    auto task = safe_coro();
    task.handle().resume();
    coro::Result<int> result = task.get();
    assert(result.is_ok());
    assert(result.value() == 100);
}

void test_safetask_error() {
    auto task = safe_error_coro();
    task.handle().resume();
    coro::Result<int> result = task.get();
    assert(result.is_err());
    assert(result.error().code() == 5);
}
```

- [ ] **Step 2: Add SafeTask<T> to coroutine.h**

```cpp
// Add to include/coro/coroutine.h

template<typename T>
class SafeTask {
public:
    using promise_type = SafeTaskPromise<T>;

    SafeTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
    SafeTask(SafeTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    SafeTask(const SafeTask&) = delete;
    ~SafeTask() { if (handle_) handle_.destroy(); }

    bool is_done() const { return handle_ && handle_.done(); }
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    coro::Result<T> get() {
        if (!handle_ || !handle_.done()) {
            return coro::Error(-1, "Task not completed");
        }
        return handle_.promise().get_result();
    }

    bool await_ready() { return handle_ && handle_.done(); }
    void await_suspend(std::coroutine_handle<> awaiting) {
        handle_.promise().set_awaiter(awaiting);
    }
    coro::Result<T> await_resume() { return get(); }

private:
    std::coroutine_handle<promise_type> handle_;
};

template<typename T>
class SafeTaskPromise {
public:
    void* operator new(size_t size) {
        return GetGlobalFramePool().Allocate(size);
    }

    void operator delete(void* ptr) {
        GetGlobalFramePool().Deallocate(ptr);
    }

    SafeTask<T> get_return_object() {
        return SafeTask<T>(std::coroutine_handle<SafeTaskPromise>::from_promise(*this));
    }

    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_value(T value) {
        result_ = coro::Result<T>(std::move(value));
        if (awaiter_) awaiter_.resume();
    }

    void return_value(coro::Error error) {
        result_ = coro::Result<T>(std::move(error));
        if (awaiter_) awaiter_.resume();
    }

    void unhandled_exception() {
        // Catch exception and convert to error
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            result_ = coro::Result<T>(coro::Error(-2, e.what()));
        } catch (...) {
            result_ = coro::Result<T>(coro::Error(-3, "unknown exception"));
        }
        if (awaiter_) awaiter_.resume();
    }

    coro::Result<T> get_result() { return std::move(result_); }
    void set_awaiter(std::coroutine_handle<> h) { awaiter_ = h; }

    void set_meta(CoroutineMeta* m) { meta_ = m; }
    CoroutineMeta* meta() const { return meta_; }

private:
    coro::Result<T> result_;
    std::coroutine_handle<> awaiter_;
    CoroutineMeta* meta_{nullptr};

    static FramePool& GetGlobalFramePool() {
        static FramePool pool;
        static std::once_flag init_flag;
        std::call_once(init_flag, [] { pool.Init(8 * 1024, 32); });
        return pool;
    }
};
```

- [ ] **Step 3: Run test**

Run: `cd g:/bthread && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
cd g:/bthread && git add include/coro/coroutine.h tests/coroutine_test.cpp && git commit -m "feat(coro): add SafeTask<T> with Result<T> error handling"
```

---

## Task 10: sleep() Function

**Files:**
- Modify: `include/coro/coroutine.h`
- Modify: `src/coro/scheduler.cpp`

- [ ] **Step 1: Write failing test for sleep()**

```cpp
// Add to tests/coroutine_test.cpp
coro::Task<int> sleep_coro() {
    auto start = std::chrono::steady_clock::now();
    co_await coro::sleep(std::chrono::milliseconds(100));
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    co_return static_cast<int>(elapsed);
}

void test_sleep() {
    coro::CoroutineScheduler::Instance().Init();

    auto task = coro::co_spawn(sleep_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int elapsed = task.get();
    assert(elapsed >= 100);  // At least 100ms passed
    assert(elapsed < 200);   // But not too long

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd g:/bthread && cmake --build build --target coroutine_test 2>&1 || echo "Build expected to fail"`
Expected: Build fails with "sleep not found"

- [ ] **Step 3: Add sleep() to coroutine.h**

```cpp
// Add to include/coro/coroutine.h
#include <chrono>

namespace coro {

// Sleep awaiter - suspends coroutine for specified duration
class SleepAwaiter {
public:
    explicit SleepAwaiter(std::chrono::milliseconds duration)
        : duration_(duration) {}

    bool await_ready() { return false; }

    bool await_suspend(std::coroutine_handle<> h);

    void await_resume() {}

private:
    std::chrono::milliseconds duration_;
};

// Sleep function
inline SleepAwaiter sleep(std::chrono::milliseconds duration) {
    return SleepAwaiter(duration);
}

} // namespace coro
```

- [ ] **Step 4: Add sleep implementation to scheduler**

```cpp
// Add to src/coro/scheduler.cpp
#include <map>
#include <condition_variable>

namespace coro {

// Simple timer system for sleep
static std::mutex sleep_mutex;
static std::condition_variable sleep_cv;
static std::map<std::chrono::steady_clock::time_point, CoroutineMeta*> sleep_queue_;
static std::atomic<bool> sleep_thread_running_{false};
static std::thread sleep_thread_;

void StartSleepThread() {
    if (sleep_thread_running_.exchange(true)) return;

    sleep_thread_ = std::thread([] {
        while (sleep_thread_running_.load()) {
            std::unique_lock<std::mutex> lock(sleep_mutex);

            if (sleep_queue_.empty()) {
                sleep_cv.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            auto it = sleep_queue_.begin();

            while (it != sleep_queue_.end() && it->first <= now) {
                CoroutineMeta* meta = it->second;
                meta->state = CoroutineMeta::READY;
                CoroutineScheduler::Instance().EnqueueCoroutine(meta);
                it = sleep_queue_.erase(it);
            }

            if (!sleep_queue_.empty()) {
                auto next_time = sleep_queue_.begin()->first;
                auto wait_duration = next_time - std::chrono::steady_clock::now();
                if (wait_duration > std::chrono::milliseconds(0)) {
                    sleep_cv.wait_for(lock, wait_duration);
                }
            }
        }
    });
}

bool SleepAwaiter::await_suspend(std::coroutine_handle<> h) {
    CoroutineMeta* meta = current_coro_meta();
    if (!meta) {
        // Not in scheduler context, use blocking sleep
        std::this_thread::sleep_for(duration_);
        return false;
    }

    meta->state = CoroutineMeta::SUSPENDED;

    // Calculate wake time
    auto wake_time = std::chrono::steady_clock::now() + duration_;

    {
        std::lock_guard<std::mutex> lock(sleep_mutex);
        sleep_queue_[wake_time] = meta;
    }
    sleep_cv.notify_one();

    // Ensure sleep thread is running
    static std::once_flag sleep_init;
    std::call_once(sleep_init, StartSleepThread);

    return true;  // Suspend
}

} // namespace coro
```

- [ ] **Step 5: Run test**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
cd g:/bthread && git add include/coro/coroutine.h src/coro/scheduler.cpp tests/coroutine_test.cpp && git commit -m "feat(coro): add sleep() function for coroutine delays"
```

---

## Task 11: Integration Tests

**Files:**
- Modify: `tests/coroutine_test.cpp`

- [ ] **Step 1: Write stress test**

```cpp
// Add to tests/coroutine_test.cpp
coro::Task<void> stress_worker(coro::CoMutex& m, std::atomic<int>& counter, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        co_await m.lock();
        counter++;
        m.unlock();
        co_await coro::yield();
    }
}

void test_stress() {
    coro::CoroutineScheduler::Instance().Init();

    coro::CoMutex mutex;
    std::atomic<int> counter{0};
    int iterations = 100;
    int workers = 10;

    std::vector<coro::Task<void>> tasks;
    for (int i = 0; i < workers; ++i) {
        tasks.push_back(coro::co_spawn(stress_worker(mutex, counter, iterations)));
    }

    // Wait for all
    for (auto& t : tasks) {
        while (!t.is_done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    assert(counter.load() == workers * iterations);

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 2: Write nested coroutine test**

```cpp
coro::Task<int> inner_coro() {
    co_return 10;
}

coro::Task<int> outer_coro() {
    int inner_result = co_await coro::co_spawn(inner_coro());
    co_return inner_result + 5;
}

void test_nested_coro() {
    coro::CoroutineScheduler::Instance().Init();

    auto task = coro::co_spawn(outer_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 15);

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 3: Write detached coroutine test**

```cpp
std::atomic<int> detached_counter{0};

coro::Task<void> detached_coro(int id) {
    co_await coro::sleep(std::chrono::milliseconds(50));
    detached_counter++;
}

void test_detached_coro() {
    coro::CoroutineScheduler::Instance().Init();

    detached_counter = 0;

    // Spawn detached coroutines (fire-and-forget)
    for (int i = 0; i < 5; ++i) {
        coro::co_spawn_detached(detached_coro(i));
    }

    // Wait for them to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    assert(detached_counter.load() == 5);

    coro::CoroutineScheduler::Instance().Shutdown();
}
```

- [ ] **Step 4: Run all tests**

Run: `cd g:/bthread && cmake --build build --target coroutine_test && ./build/tests/coroutine_test.exe`
Expected: PASS (all tests)

- [ ] **Step 5: Commit**

```bash
cd g:/bthread && git add tests/coroutine_test.cpp && git commit -m "test(coro): add stress, nested, and detached coroutine tests"
```

---

## Task 12: Final Integration

**Files:**
- Modify: `CMakeLists.txt` (ensure all files linked)
- Create: `demo/coro_demo.cpp`
- Modify: `demo/CMakeLists.txt`

- [ ] **Step 1: Create demo**

```cpp
// demo/coro_demo.cpp
#include "coro/coroutine.h"
#include "coro/scheduler.h"
#include "coro/mutex.h"
#include "coro/cond.h"
#include "coro/cancel.h"

#include <iostream>
#include <atomic>

coro::Task<void> demo_task(int id, coro::CoMutex& m, std::atomic<int>& counter) {
    std::cerr << "Task " << id << " starting\n";

    co_await m.lock();
    counter++;
    std::cerr << "Task " << id << " incremented counter to " << counter << "\n";
    m.unlock();

    co_await coro::yield();

    std::cerr << "Task " << id << " done\n";
}

int main() {
    setvbuf(stderr, nullptr, _IONBF, 0);

    coro::CoroutineScheduler::Instance().Init();

    coro::CoMutex mutex;
    std::atomic<int> counter{0};

    for (int i = 0; i < 5; ++i) {
        coro::co_spawn(demo_task(i, mutex, counter));
    }

    // Wait
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cerr << "Final counter: " << counter << "\n";

    coro::CoroutineScheduler::Instance().Shutdown();

    return 0;
}
```

- [ ] **Step 2: Add to demo CMakeLists.txt**

```cmake
# Add to demo/CMakeLists.txt
add_executable(coro_demo coro_demo.cpp)
target_link_libraries(coro_demo PRIVATE coro bthread)
```

- [ ] **Step 3: Build and run demo**

Run: `cd g:/bthread && cmake -B build -S . && cmake --build build --target coro_demo && ./build/demo/coro_demo.exe`
Expected: Demo runs, shows tasks incrementing counter

- [ ] **Step 4: Final commit**

```bash
cd g:/bthread && git add demo/coro_demo.cpp demo/CMakeLists.txt CMakeLists.txt && git commit -m "feat(coro): complete coroutine pool implementation with demo"
```

---

## Summary

This plan implements a complete C++20 coroutine pool with:

1. **Error and Result<T>** - Type-safe error handling
2. **FramePool** - Efficient coroutine frame memory management
3. **CoroutineMeta** - Coroutine lifecycle tracking
4. **CoroutineScheduler** - M:N scheduling with dedicated workers
5. **Task<T>/SafeTask<T>** - Two API styles for coroutine returns
6. **CoMutex** - Coroutine-safe mutex
7. **CoCond** - Coroutine condition variable
8. **Cancellation** - Cooperative cancellation support
9. **yield()** - Explicit yield point
10. **sleep()** - Coroutine sleep with timer-based wake-up
11. **co_spawn_detached()** - Fire-and-forget coroutine spawning
12. **Tests** - Comprehensive test coverage including detached coroutines

Each task follows TDD: write failing test, implement, verify test passes, commit.

**Architecture Note:** This implementation uses dedicated coroutine worker threads (not shared with bthread workers) for Phase 1 simplicity. Future versions may integrate with bthread's Worker pool for unified scheduling.