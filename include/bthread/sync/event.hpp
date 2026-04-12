// include/bthread/sync/event.hpp
#pragma once

#include <atomic>
#include <coroutine>
#include <chrono>

#include "bthread/core/task_meta_base.hpp"
#include "bthread/queue/intrusive_waiter_queue.hpp"

namespace bthread {

/**
 * @brief Event (modernized Butex) - simple synchronization primitive.
 *
 * An event can be in one of two states: set or not set.
 * Tasks can wait for the event to become set.
 *
 * Provides:
 * - Blocking wait() for bthread/pthread contexts
 * - Awaitable wait_async() for coroutine contexts
 * - Automatic and manual reset modes
 *
 * Thread Safety:
 * - All methods are thread-safe
 *
 * Usage:
 * ```cpp
 * bthread::Event event(false);  // Initially not set
 *
 * // Waiter:
 * event.wait();  // Blocks until event is set
 *
 * // Signaler:
 * event.set();   // Wakes all waiters
 * ```
 */
class Event {
public:
    /**
     * @brief Construct an event with initial state.
     * @param initial_set If true, event starts in set state
     * @param auto_reset If true, event automatically resets after one waiter wakes
     */
    explicit Event(bool initial_set = false, bool auto_reset = false);
    ~Event();

    // Disable copy and move
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    // ========== State Query ==========

    /**
     * @brief Check if event is set.
     */
    bool is_set() const {
        return state_.load(std::memory_order_acquire);
    }

    // ========== Blocking Wait (bthread/pthread) ==========

    /**
     * @brief Wait for the event to become set.
     * Returns immediately if already set.
     */
    void wait();

    /**
     * @brief Wait for the event with timeout.
     * @param timeout Maximum time to wait
     * @return true if event became set, false if timeout expired
     */
    bool wait_for(std::chrono::milliseconds timeout);

    // ========== Awaitable Wait (coroutine) ==========

    /**
     * @brief Awaiter for waiting on the event from a coroutine.
     */
    class WaitAwaiter {
    public:
        explicit WaitAwaiter(Event& event) : event_(event) {}

        /// Check if event is already set
        bool await_ready() {
            return event_.is_set();
        }

        /// Suspend coroutine if event not set
        bool await_suspend(std::coroutine_handle<> h);

        /// Called when coroutine resumes
        void await_resume() {}

    private:
        Event& event_;
    };

    /**
     * @brief Get an awaiter for waiting from a coroutine.
     * Usage: co_await event.wait_async();
     */
    WaitAwaiter wait_async() {
        return WaitAwaiter(*this);
    }

    // ========== State Change ==========

    /**
     * @brief Set the event.
     * Wakes all waiting tasks.
     */
    void set();

    /**
     * @brief Reset the event to not-set state.
     */
    void reset();

private:
    std::atomic<bool> state_{false};
    bool auto_reset_{false};

    // Intrusive waiter queue (zero-allocation)
    IntrusiveWaiterQueue waiter_queue_;

    // Helper methods (lock-free)
    void enqueue_waiter(TaskMetaBase* task);
    TaskMetaBase* dequeue_waiter();
    void wake_all_waiters();
};

} // namespace bthread