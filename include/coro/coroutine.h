// include/coro/coroutine.h
#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include <mutex>
#include <chrono>
#include "coro/result.h"
#include "coro/meta.h"
#include "coro/frame_pool.h"

namespace coro {

// Forward declarations
class CoroutineScheduler;
template<typename T> class SafeTask;
template<typename T> class SafeTaskPromise;
template<typename T> class Task;
template<typename T> class TaskPromise;

// Shared global frame pool for coroutine promise allocations
inline FramePool& GetGlobalFramePool() {
    static FramePool pool;
    static std::once_flag init_flag;
    std::call_once(init_flag, [] { pool.Init(8 * 1024, 32); });
    return pool;
}

// Declare explicit specializations BEFORE any usage to prevent primary template instantiation with void
template<> class Task<void>;
template<> class TaskPromise<void>;

// TaskPromise<T> - promise type for Task<T> (primary template)
template<typename T>
class TaskPromise {
public:
    TaskPromise() = default;

    static void* operator new(size_t size) { return GetGlobalFramePool().Allocate(size); }
    static void operator delete(void* ptr) { return GetGlobalFramePool().Deallocate(ptr); }

    Task<T> get_return_object() {
        return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_value(T value) {
        result_ = std::move(value);
        if (awaiter_) awaiter_.resume();
    }

    void unhandled_exception() {
        exception_ = std::current_exception();
        if (awaiter_) awaiter_.resume();
    }

    T get_result() {
        if (exception_) std::rethrow_exception(exception_);
        return std::move(result_);
    }

    void set_awaiter(std::coroutine_handle<> h) { awaiter_ = h; }

    void set_meta(CoroutineMeta* m) { meta_ = m; }
    CoroutineMeta* meta() const { return meta_; }

private:
    T result_{};
    std::exception_ptr exception_;
    std::coroutine_handle<> awaiter_;
    CoroutineMeta* meta_{nullptr};
};

// Task<T> - exception-based coroutine return type (primary template)
template<typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;

    Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Task(const Task&) = delete;
    ~Task() { if (handle_) handle_.destroy(); }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool is_done() const { return handle_ && handle_.done(); }
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    T get() {
        if (!handle_) throw std::runtime_error("Task has no handle");
        if (!handle_.done()) throw std::runtime_error("Task not completed");
        return handle_.promise().get_result();
    }

    bool await_ready() { return handle_ && handle_.done(); }
    void await_suspend(std::coroutine_handle<> awaiting) { handle_.promise().set_awaiter(awaiting); }
    T await_resume() { return get(); }

private:
    std::coroutine_handle<promise_type> handle_;
};

// TaskPromise<void> - specialization for void (declaration only, body defined after Task<void>)
template<>
class TaskPromise<void> {
public:
    static void* operator new(size_t size) { return GetGlobalFramePool().Allocate(size); }
    static void operator delete(void* ptr) { GetGlobalFramePool().Deallocate(ptr); }

    // Declaration only - body defined after Task<void> is complete
    Task<void> get_return_object();

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() { if (awaiter_) awaiter_.resume(); }

    void unhandled_exception() {
        exception_ = std::current_exception();
        if (awaiter_) awaiter_.resume();
    }

    void get_result() { if (exception_) std::rethrow_exception(exception_); }
    void set_awaiter(std::coroutine_handle<> h) { awaiter_ = h; }

    void set_meta(CoroutineMeta* m) { meta_ = m; }
    CoroutineMeta* meta() const { return meta_; }

private:
    std::exception_ptr exception_;
    std::coroutine_handle<> awaiter_;
    CoroutineMeta* meta_{nullptr};
};

// Task<void> - specialization for void
template<>
class Task<void> {
public:
    using promise_type = TaskPromise<void>;

    Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Task(const Task&) = delete;
    ~Task() { if (handle_) handle_.destroy(); }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool is_done() const { return handle_ && handle_.done(); }
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    void get() {
        if (!handle_) throw std::runtime_error("Task has no handle");
        if (!handle_.done()) throw std::runtime_error("Task not completed");
        handle_.promise().get_result();
    }

    bool await_ready() { return handle_ && handle_.done(); }
    void await_suspend(std::coroutine_handle<> awaiting) { handle_.promise().set_awaiter(awaiting); }
    void await_resume() { get(); }

private:
    std::coroutine_handle<promise_type> handle_;
};

// TaskPromise<void>::get_return_object() definition - now Task<void> is complete
inline Task<void> TaskPromise<void>::get_return_object() {
    return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}

// === SafeTask<T> - error-safe coroutine return type ===

// Forward declare SafeTaskPromise before SafeTask uses it
template<typename T> class SafeTaskPromise;

// SafeTask<T> - uses Result<T> for error handling instead of exceptions
template<typename T>
class SafeTask {
    // Prevent SafeTask<Error> - would cause overload ambiguity in SafeTaskPromise
    // between return_value(T) and return_value(Error)
    static_assert(!std::is_same_v<T, coro::Error>, "SafeTask<Error> is not allowed due to overload ambiguity");

public:
    using promise_type = SafeTaskPromise<T>;

    SafeTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
    SafeTask(SafeTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    SafeTask(const SafeTask&) = delete;
    ~SafeTask() { if (handle_) handle_.destroy(); }

    SafeTask& operator=(SafeTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool is_done() const { return handle_ && handle_.done(); }
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    // Returns Result<T> instead of T - no exceptions thrown
    [[nodiscard]] coro::Result<T> get() {
        if (!handle_) {
            return coro::Result<T>(coro::Error(-1, "Task has no handle"));
        }
        if (!handle_.done()) {
            return coro::Result<T>(coro::Error(-2, "Task not completed"));
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

// SafeTaskPromise<T> - promise type for SafeTask<T>
// Key differences from TaskPromise:
// - return_value accepts T or Error
// - unhandled_exception converts exceptions to Error
// - get_result returns Result<T>
template<typename T>
class SafeTaskPromise {
public:
    SafeTaskPromise() = default;

    static void* operator new(size_t size) { return GetGlobalFramePool().Allocate(size); }
    static void operator delete(void* ptr) { return GetGlobalFramePool().Deallocate(ptr); }

    SafeTask<T> get_return_object() {
        return SafeTask<T>(std::coroutine_handle<SafeTaskPromise<T>>::from_promise(*this));
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    // Accept value - creates Result<T> with success
    void return_value(T value) {
        result_ = coro::Result<T>(std::move(value));
        if (awaiter_) awaiter_.resume();
    }

    // Accept Error - creates Result<T> with error
    void return_value(coro::Error error) {
        result_ = coro::Result<T>(std::move(error));
        if (awaiter_) awaiter_.resume();
    }

    // Convert exceptions to Error
    void unhandled_exception() {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            result_ = coro::Result<T>(coro::Error(-3, e.what()));
        } catch (...) {
            result_ = coro::Result<T>(coro::Error(-4, "unknown exception"));
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
};

// === SafeTask<void> and SafeTaskPromise<void> specializations ===

// VoidSuccess - helper type for SafeTask<void> success case
// Usage: co_return coro::VoidSuccess{}; for success
struct VoidSuccess {};

// Declare explicit specializations before any usage
template<> class SafeTask<void>;
template<> class SafeTaskPromise<void>;

// SafeTaskPromise<void> - specialization for void
// Note: Cannot have both return_void and return_value in C++ coroutines.
// Uses return_value with VoidSuccess for success, Error for error.
template<>
class SafeTaskPromise<void> {
public:
    SafeTaskPromise() = default;

    static void* operator new(size_t size) { return GetGlobalFramePool().Allocate(size); }
    static void operator delete(void* ptr) { return GetGlobalFramePool().Deallocate(ptr); }

    // Declaration only - body defined after SafeTask<void> is complete
    SafeTask<void> get_return_object();

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    // Success case - co_return VoidSuccess{}; or co_return Result<void>();
    void return_value(VoidSuccess) {
        result_ = coro::Result<void>();
        if (awaiter_) awaiter_.resume();
    }

    // Also accept Result<void> directly
    void return_value(coro::Result<void> res) {
        result_ = std::move(res);
        if (awaiter_) awaiter_.resume();
    }

    // Error case - co_return Error(...);
    void return_value(coro::Error error) {
        result_ = coro::Result<void>(std::move(error));
        if (awaiter_) awaiter_.resume();
    }

    // Convert exceptions to Error
    void unhandled_exception() {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            result_ = coro::Result<void>(coro::Error(-3, e.what()));
        } catch (...) {
            result_ = coro::Result<void>(coro::Error(-4, "unknown exception"));
        }
        if (awaiter_) awaiter_.resume();
    }

    coro::Result<void> get_result() { return std::move(result_); }
    void set_awaiter(std::coroutine_handle<> h) { awaiter_ = h; }

    void set_meta(CoroutineMeta* m) { meta_ = m; }
    CoroutineMeta* meta() const { return meta_; }

private:
    coro::Result<void> result_;
    std::coroutine_handle<> awaiter_;
    CoroutineMeta* meta_{nullptr};
};

// SafeTask<void> - specialization for void
template<>
class SafeTask<void> {
public:
    using promise_type = SafeTaskPromise<void>;

    SafeTask(std::coroutine_handle<promise_type> h) : handle_(h) {}
    SafeTask(SafeTask&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    SafeTask(const SafeTask&) = delete;
    ~SafeTask() { if (handle_) handle_.destroy(); }

    SafeTask& operator=(SafeTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool is_done() const { return handle_ && handle_.done(); }
    std::coroutine_handle<promise_type> handle() const { return handle_; }

    // Returns Result<void> - no exceptions thrown
    [[nodiscard]] coro::Result<void> get() {
        if (!handle_) {
            return coro::Result<void>(coro::Error(-1, "Task has no handle"));
        }
        if (!handle_.done()) {
            return coro::Result<void>(coro::Error(-2, "Task not completed"));
        }
        return handle_.promise().get_result();
    }

    bool await_ready() { return handle_ && handle_.done(); }
    void await_suspend(std::coroutine_handle<> awaiting) {
        handle_.promise().set_awaiter(awaiting);
    }
    coro::Result<void> await_resume() { return get(); }

private:
    std::coroutine_handle<promise_type> handle_;
};

// SafeTaskPromise<void>::get_return_object() definition - now SafeTask<void> is complete
inline SafeTask<void> SafeTaskPromise<void>::get_return_object() {
    return SafeTask<void>(std::coroutine_handle<SafeTaskPromise<void>>::from_promise(*this));
}

// === Yield support ===

/**
 * @brief Awaiter for yielding the current coroutine.
 * Suspends the coroutine and re-queues it for later execution,
 * allowing other coroutines to run.
 */
class YieldAwaiter {
public:
    // Always suspend - the purpose is to yield
    bool await_ready() noexcept { return false; }

    // Implementation in coroutine.cpp - needs scheduler.h
    bool await_suspend(std::coroutine_handle<> h) noexcept;

    // Nothing to return on resume
    void await_resume() noexcept {}
};

/**
 * @brief Yield the current coroutine, allowing others to run.
 * Usage: co_await coro::yield();
 * The coroutine is suspended and re-queued for execution.
 */
inline YieldAwaiter yield() { return YieldAwaiter{}; }

// === Sleep support ===

/**
 * @brief Awaiter for sleeping the current coroutine.
 * Suspends the coroutine for a specified duration and uses
 * timer-based wake-up for efficient waiting.
 */
class SleepAwaiter {
public:
    explicit SleepAwaiter(std::chrono::milliseconds duration)
        : duration_(duration) {}

    // Always suspend - we need to schedule wake-up
    bool await_ready() noexcept { return false; }

    // Implementation in scheduler.cpp - needs timer thread
    bool await_suspend(std::coroutine_handle<> h) noexcept;

    // Nothing to return on resume
    void await_resume() noexcept {}

private:
    std::chrono::milliseconds duration_;
};

/**
 * @brief Sleep the current coroutine for a specified duration.
 * Usage: co_await coro::sleep(std::chrono::milliseconds(100));
 * The coroutine is suspended and woken up by a timer thread.
 *
 * @param duration The duration to sleep (in milliseconds)
 * @return SleepAwaiter that can be co_awaited
 */
inline SleepAwaiter sleep(std::chrono::milliseconds duration) {
    return SleepAwaiter(duration);
}

} // namespace coro