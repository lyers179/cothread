// include/coro/cond.h
#pragma once

#include <coroutine>
#include <mutex>
#include "coro/meta.h"
#include "coro/mutex.h"

namespace coro {

/**
 * @brief Coroutine condition variable for synchronization with CoMutex.
 *
 * Thread Safety:
 * - wait(): Safe to call from multiple coroutines concurrently.
 * - signal(): Thread-safe, wakes one waiter.
 * - broadcast(): Thread-safe, wakes all waiters.
 *
 * Usage:
 * - Use co_await cond.wait(mutex) to wait for condition (atomically unlocks mutex).
 * - Call cond.signal() to wake one waiting coroutine.
 * - Call cond.broadcast() to wake all waiting coroutines.
 *
 * Design:
 * - Suspended coroutines are added to a waiters queue (mutex-protected for MPMC safety).
 * - wait() atomically unlocks mutex and suspends, re-acquires on resume.
 * - signal() wakes one waiter, broadcast() wakes all waiters.
 *
 * Typical pattern (producer-consumer):
 * @code
 * // Consumer:
 * co_await mutex.lock();
 * while (condition_not_met) {
 *     co_await cond.wait(mutex);  // unlocks mutex, waits, re-locks on resume
 * }
 * // ... process data ...
 * mutex.unlock();
 *
 * // Producer:
 * co_await mutex.lock();
 * // ... produce data, set condition ...
 * cond.signal();  // or broadcast() for multiple consumers
 * mutex.unlock();
 * @endcode
 */
class CoCond {
public:
    CoCond();
    ~CoCond();

    /**
     * @brief Awaitable object for waiting on the condition variable.
     *
     * Usage: co_await cond.wait(mutex);
     *
     * Behavior:
     * - Atomically unlocks the mutex and suspends the coroutine.
     * - When signaled (signal/broadcast), the coroutine is resumed.
     * - On resume, the mutex is re-acquired before returning from co_await.
     *
     * IMPORTANT: The caller must hold the mutex before calling wait().
     */
    class WaitAwaiter {
    public:
        WaitAwaiter(CoCond& cond, CoMutex& mutex)
            : cond_(cond), mutex_(mutex) {}

        /**
         * @brief Always suspend - condition variables always need to wait.
         * @return false - always suspend.
         */
        bool await_ready() { return false; }

        /**
         * @brief Suspend coroutine, unlock mutex, and add to waiters queue.
         * @param h The coroutine handle to suspend.
         * @return true if coroutine should suspend.
         */
        bool await_suspend(std::coroutine_handle<> h);

        /**
         * @brief Called when coroutine resumes - re-acquire mutex.
         *
         * The mutex must be held before the caller can proceed.
         */
        void await_resume();

    private:
        CoCond& cond_;
        CoMutex& mutex_;
        CoroutineMeta* meta_{nullptr};
    };

    /**
     * @brief Get an awaiter for waiting on the condition with a mutex.
     * Usage: co_await cond.wait(mutex);
     *
     * PRECONDITION: The caller must hold the mutex lock.
     */
    WaitAwaiter wait(CoMutex& mutex) { return WaitAwaiter(*this, mutex); }

    /**
     * @brief Wake one waiting coroutine.
     *
     * If there are waiting coroutines, one is woken and will attempt to
     * re-acquire the mutex. If no waiters, this is a no-op.
     *
     * IMPORTANT: For proper thread safety, signal() should typically be called
     * while holding the associated mutex. This ensures that:
     * - The condition being signaled cannot change between evaluation and signal
     * - The waiting coroutine cannot miss the signal (race condition prevention)
     * However, if signal() is called without the mutex, it is still safe from
     * a data perspective (no undefined behavior), but may result in missed
     * wakeups or spurious wakeups.
     */
    void signal();

    /**
     * @brief Wake all waiting coroutines.
     *
     * All waiting coroutines are woken and will attempt to re-acquire the mutex
     * one by one. If no waiters, this is a no-op.
     *
     * IMPORTANT: For proper thread safety, broadcast() should typically be called
     * while holding the associated mutex. This ensures that:
     * - The condition being broadcast cannot change between evaluation and broadcast
     * - Waiting coroutines cannot miss the broadcast (race condition prevention)
     * However, if broadcast() is called without the mutex, it is still safe from
     * a data perspective (no undefined behavior), but may result in missed
     * wakeups or spurious wakeups.
     */
    void broadcast();

private:
    CoroutineQueue waiters_;
    std::mutex waiters_mutex_;  ///< Protects waiters_ for MPMC safety
};

} // namespace coro