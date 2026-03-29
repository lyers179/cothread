// include/coro/coroutine.h
#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include <mutex>
#include "coro/result.h"
#include "coro/meta.h"
#include "coro/frame_pool.h"

namespace coro {

// Forward declarations
class CoroutineScheduler;
template<typename T> class SafeTask;
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

} // namespace coro