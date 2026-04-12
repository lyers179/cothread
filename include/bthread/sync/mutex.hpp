// include/bthread/sync/mutex.hpp
#pragma once

#include <atomic>
#include <mutex>
#include <coroutine>

#include "bthread/core/task_meta_base.hpp"
#include "bthread/queue/mpsc_queue.hpp"  // WaiterQueue uses waiter_next policy

// Forward declarations
namespace bthread {
class Scheduler;
}

namespace coro {
struct CoroutineMeta;
}

namespace bthread {

/**
 * @brief Unified mutex that works for both bthread and coroutine contexts.
 *
 * Provides:
 * - Blocking lock() for bthread/pthread contexts
 * - Awaitable lock_async() for coroutine contexts
 * - Non-blocking try_lock() for all contexts
 *
 * Thread Safety:
 * - All methods are thread-safe
 * - Can be called from any context (bthread, coroutine, or pthread)
 *
 * Usage:
 * ```cpp
 * bthread::Mutex mutex;
 *
 * // From bthread/pthread:
 * mutex.lock();
 * // ... critical section ...
 * mutex.unlock();
 *
 * // From coroutine:
 * co_await mutex.lock_async();
 * // ... critical section ...
 * mutex.unlock();
 * ```
 */
class Mutex {
public:
    Mutex();
    ~Mutex();

    // Disable copy and move
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    // ========== Blocking Lock (bthread/pthread) ==========

    /**
     * @brief Acquire the lock, blocking until available.
     * For bthread: Uses Butex for efficient waiting
     * For pthread: Uses native mutex
     */
    void lock();

    /**
     * @brief Try to acquire the lock without blocking.
     * @return true if lock acquired, false if already locked
     */
    bool try_lock();

    /**
     * @brief Release the lock.
     * Wakes one waiting task if any.
     */
    void unlock();

    // ========== Awaitable Lock (coroutine) ==========

    /**
     * @brief Get an awaiter for acquiring the lock from a coroutine.
     * Usage: co_await mutex.lock_async();
     * @return LockAwaiter that suspends the coroutine if lock is held
     */
    class LockAwaiter {
    public:
        explicit LockAwaiter(Mutex& mutex) : mutex_(mutex) {}

        /// Check if lock can be acquired immediately
        bool await_ready() {
            return mutex_.try_lock();
        }

        /// Suspend coroutine and add to waiters queue
        bool await_suspend(std::coroutine_handle<> h);

        /// Called when coroutine resumes - lock is now held
        void await_resume() {}

    private:
        Mutex& mutex_;
    };

    LockAwaiter lock_async() { return LockAwaiter(*this); }

private:
    // State bits
    static constexpr uint32_t LOCKED = 1;
    static constexpr uint32_t HAS_WAITERS = 2;

    std::atomic<uint32_t> state_{0};
    std::atomic<uint32_t> pending_wake_{0};  // Optimization 3: Prevent duplicate wake

    // Intrusive waiter queue for coroutine waiters (zero-allocation)
    WaiterQueue waiter_queue_;  // Uses waiter_next field via policy

    // Native mutex for pthread context
    void* native_mutex_{nullptr};

    // Butex for bthread context (atomic for lock-free init)
    std::atomic<void*> butex_{nullptr};

    // Friend classes for private access
    friend class CondVar;

    // Helper methods
    void lock_bthread();
    void lock_pthread();
    void unlock_bthread();
    void unlock_pthread();
    void enqueue_waiter(TaskMetaBase* task);
    TaskMetaBase* dequeue_waiter();
};

} // namespace bthread