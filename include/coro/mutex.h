// include/coro/mutex.h
#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include "coro/meta.h"

namespace coro {

/**
 * @brief Coroutine-safe mutex that suspends coroutines instead of blocking threads.
 *
 * Thread Safety:
 * - lock(): Safe to call from multiple coroutines concurrently.
 * - try_lock(): Thread-safe, atomic operation.
 * - unlock(): Thread-safe, wakes waiters correctly.
 *
 * Usage:
 * - Use co_await mutex.lock() to acquire the lock from a coroutine.
 * - Use mutex.try_lock() for non-blocking attempts (returns true if acquired).
 * - Call mutex.unlock() to release the lock and wake one waiting coroutine.
 *
 * Design:
 * - Uses atomic state with LOCKED and HAS_WAITERS flags.
 * - Suspended coroutines are added to a waiters queue (mutex-protected for MPMC safety).
 * - unlock() wakes one waiting coroutine and passes the lock to it.
 */
class CoMutex {
public:
    CoMutex();
    ~CoMutex();

    /**
     * @brief Awaitable object for acquiring the mutex lock.
     *
     * Usage: co_await mutex.lock();
     *
     * Behavior:
     * - If mutex is unlocked, acquires immediately without suspending.
     * - If mutex is locked, suspends the coroutine and adds it to waiters queue.
     * - When the lock becomes available, the coroutine is resumed with lock held.
     */
    class LockAwaiter {
    public:
        explicit LockAwaiter(CoMutex& mutex) : mutex_(mutex) {}

        /**
         * @brief Check if lock can be acquired immediately.
         * @return true if lock acquired, false if need to suspend.
         */
        bool await_ready() {
            return mutex_.try_lock();
        }

        /**
         * @brief Suspend coroutine and add to waiters queue.
         * @param h The coroutine handle to suspend.
         * @return true if coroutine should suspend, false if lock acquired.
         */
        bool await_suspend(std::coroutine_handle<> h);

        /**
         * @brief Called when coroutine resumes - lock is now held.
         */
        void await_resume() {}

    private:
        CoMutex& mutex_;
    };

    /**
     * @brief Get an awaiter for acquiring the lock.
     * Usage: co_await mutex.lock();
     */
    LockAwaiter lock() { return LockAwaiter(*this); }

    /**
     * @brief Try to acquire the lock without suspending.
     * @return true if lock acquired, false if lock is held by another coroutine.
     */
    bool try_lock();

    /**
     * @brief Release the lock and wake one waiting coroutine.
     *
     * If there are waiting coroutines, one is woken and given the lock.
     * If no waiters, the lock is simply released.
     */
    void unlock();

private:
    static constexpr uint32_t LOCKED = 1;
    static constexpr uint32_t HAS_WAITERS = 2;

    std::atomic<uint32_t> state_{0};
    CoroutineQueue waiters_;
    std::mutex waiters_mutex_;  ///< Protects waiters_ for MPMC safety
};

} // namespace coro