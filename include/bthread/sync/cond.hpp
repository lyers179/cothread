// include/bthread/sync/cond.hpp
#pragma once

#include <atomic>
#include <mutex>
#include <coroutine>
#include <chrono>

#include "bthread/core/task_meta_base.hpp"

namespace bthread {

// Forward declarations
class Mutex;

/**
 * @brief Unified condition variable that works for both bthread and coroutine contexts.
 *
 * Provides:
 * - Blocking wait() for bthread/pthread contexts
 * - Awaitable wait_async() for coroutine contexts
 * - Timed wait with timeout support
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Must be used with bthread::Mutex
 *
 * Usage:
 * ```cpp
 * bthread::Mutex mutex;
 * bthread::CondVar cond;
 *
 * // From bthread/pthread:
 * mutex.lock();
 * while (!condition) {
 *     cond.wait(mutex);
 * }
 * // ... access shared resource ...
 * mutex.unlock();
 *
 * // From coroutine:
 * co_await mutex.lock_async();
 * while (!condition) {
 *     co_await cond.wait_async(mutex);
 * }
 * // ... access shared resource ...
 * mutex.unlock();
 * ```
 */
class CondVar {
public:
    CondVar();
    ~CondVar();

    // Disable copy and move
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;
    CondVar(CondVar&&) = delete;
    CondVar& operator=(CondVar&&) = delete;

    // ========== Blocking Wait (bthread/pthread) ==========

    /**
     * @brief Wait on the condition variable.
     * Atomically releases the mutex and blocks until notified.
     * Re-acquires the mutex before returning.
     * @param mutex The mutex to release/acquire
     */
    void wait(Mutex& mutex);

    /**
     * @brief Wait on the condition variable with timeout.
     * @param mutex The mutex to release/acquire
     * @param timeout Maximum time to wait
     * @return true if notified, false if timeout expired
     */
    bool wait_for(Mutex& mutex, std::chrono::milliseconds timeout);

    // ========== Awaitable Wait (coroutine) ==========

    /**
     * @brief Awaiter for waiting on the condition variable from a coroutine.
     */
    class WaitAwaiter {
    public:
        WaitAwaiter(CondVar& cond, Mutex& mutex)
            : cond_(cond), mutex_(mutex) {}

        /// Always suspend - we need to release the mutex and wait
        bool await_ready() { return false; }

        /// Suspend coroutine and release mutex
        bool await_suspend(std::coroutine_handle<> h);

        /// Re-acquire mutex before resuming
        void await_resume();

    private:
        CondVar& cond_;
        Mutex& mutex_;
    };

    /**
     * @brief Get an awaiter for waiting from a coroutine.
     * Usage: co_await cond.wait_async(mutex);
     */
    WaitAwaiter wait_async(Mutex& mutex) {
        return WaitAwaiter(*this, mutex);
    }

    // ========== Notification ==========

    /**
     * @brief Wake one waiting task.
     */
    void notify_one();

    /**
     * @brief Wake all waiting tasks.
     */
    void notify_all();

private:
    // Waiters queue
    std::mutex waiters_mutex_;
    struct WaiterNode {
        TaskMetaBase* task;
        WaiterNode* next;
    };
    WaiterNode* waiter_head_{nullptr};
    WaiterNode* waiter_tail_{nullptr};

    // Native condition variable for pthread context
    void* native_cond_{nullptr};

    // Helper methods
    void enqueue_waiter(TaskMetaBase* task);
    TaskMetaBase* dequeue_waiter();
    void wait_pthread(Mutex& mutex);
};

} // namespace bthread