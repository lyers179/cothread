// src/coro/mutex.cpp
#include "coro/mutex.h"
#include "coro/scheduler.h"
#include <cassert>
#include <stdexcept>

namespace coro {

CoMutex::CoMutex() = default;

CoMutex::~CoMutex() {
    // Critical: Destroying mutex with pending waiters leaks coroutines
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    assert(waiters_.Empty() && "Cannot destroy mutex with pending waiters");
}

bool CoMutex::try_lock() {
    uint32_t expected = 0;
    return state_.compare_exchange_strong(expected, LOCKED,
        std::memory_order_acquire, std::memory_order_relaxed);
}

bool CoMutex::LockAwaiter::await_suspend(std::coroutine_handle<> h) {
    // Get existing CoroutineMeta from current coroutine (set by CoroutineWorkerLoop)
    CoroutineMeta* meta = current_coro_meta();
    if (!meta) {
        // Critical: Not in scheduler context - this is a programming error
        // The coroutine would proceed without the lock, causing undefined behavior
        throw std::logic_error("Cannot co_await mutex.lock() outside of scheduler context");
    }

    // Try to acquire lock one more time before suspending
    if (mutex_.try_lock()) {
        return false;  // Got the lock, don't suspend
    }

    // Mark that we have waiters
    mutex_.state_.fetch_or(HAS_WAITERS, std::memory_order_release);

    // Critical: Retry lock acquisition after setting HAS_WAITERS
    // This handles the race where the lock holder releases between
    // our initial try_lock() above and us pushing to the queue.
    // Without this check, we could push to queue with no lock holder,
    // causing indefinite wait (deadlock).
    if (mutex_.try_lock()) {
        // Got the lock! Clear HAS_WAITERS if we're the only waiter.
        // The flag is harmless if left set (unlock clears it when no waiters),
        // but we optimistically clear it for cleanliness.
        mutex_.state_.fetch_and(~HAS_WAITERS, std::memory_order_release);
        return false;  // Got the lock, don't suspend
    }

    // Set state on existing meta
    meta->state.store(bthread::TaskState::SUSPENDED, std::memory_order_release);
    meta->waiting_sync = &mutex_;

    // Add to waiters queue (mutex-protected for MPMC safety)
    {
        std::lock_guard<std::mutex> lock(mutex_.waiters_mutex_);
        mutex_.waiters_.Push(meta);
    }

    return true;  // Suspend
}

void CoMutex::unlock() {
    // Debug assertion: detect unlock-without-lock
    assert((state_.load(std::memory_order_acquire) & LOCKED) &&
           "unlock() called without holding the lock");

    // Check if there are waiters before clearing LOCKED
    // This avoids the race condition: we transfer ownership atomically
    CoroutineMeta* waiter = nullptr;
    {
        std::lock_guard<std::mutex> lock(waiters_mutex_);
        waiter = waiters_.Pop();
    }

    if (waiter) {
        // Transfer lock ownership directly to waiter WITHOUT clearing LOCKED
        // This is atomic handoff - no window for another thread to steal the lock
        waiter->state.store(bthread::TaskState::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;

        CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
        // LOCKED bit remains set - the waiter now owns the lock
    } else {
        // No waiters - now safe to clear LOCKED and HAS_WAITERS flags
        state_.store(0, std::memory_order_release);
    }
}

} // namespace coro