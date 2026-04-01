// src/coro/cond.cpp
#include "coro/cond.h"
#include "coro/scheduler.h"
#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>

namespace coro {

CoCond::CoCond() = default;

CoCond::~CoCond() {
    // Critical: Destroying condition variable with pending waiters leaks coroutines
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    assert(waiters_.Empty() && "Cannot destroy CoCond with pending waiters");
}

bool CoCond::WaitAwaiter::await_suspend(std::coroutine_handle<> h) {
    // Get existing CoroutineMeta from current coroutine
    meta_ = current_coro_meta();
    if (!meta_) {
        // Critical: Not in scheduler context - this is a programming error
        throw std::logic_error("Cannot co_await cond.wait() outside of scheduler context");
    }

    // Set state on existing meta
    meta_->state.store(CoroutineMeta::SUSPENDED, std::memory_order_release);
    meta_->waiting_sync = &cond_;

    // Unlock mutex before waiting (this is the core of condition variable semantics)
    mutex_.unlock();

    // Add to waiters queue (mutex-protected for MPMC safety)
    {
        std::lock_guard<std::mutex> lock(cond_.waiters_mutex_);
        cond_.waiters_.Push(meta_);
    }

    return true;  // Suspend
}

void CoCond::WaitAwaiter::await_resume() {
    // Called when coroutine resumes - need to re-acquire mutex
    // The mutex was unlocked in await_suspend, and we must re-acquire it
    // before returning from co_await.

    // Implementation note: The current design uses a spin-yield loop for mutex
    // re-acquisition. This is a limitation because await_resume() cannot be a
    // coroutine and cannot co_await. Proper solutions would require:
    //   1. Two-phase suspension mechanism
    //   2. Ownership transfer in signal() (requires signaler to hold mutex)
    //
    // The spin-yield is acceptable because:
    //   - Condition variables are typically used with short-held mutexes
    //   - std::this_thread::yield() prevents busy-waiting
    //   - The signaling coroutine typically releases mutex immediately after signal()

    while (!mutex_.try_lock()) {
        std::this_thread::yield();
    }
    // Mutex is now held by us
}

void CoCond::signal() {
    CoroutineMeta* waiter = nullptr;
    {
        std::lock_guard<std::mutex> lock(waiters_mutex_);
        waiter = waiters_.Pop();
    }

    if (waiter) {
        waiter->state.store(CoroutineMeta::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
    }
}

void CoCond::broadcast() {
    std::vector<CoroutineMeta*> to_wake;
    {
        std::lock_guard<std::mutex> lock(waiters_mutex_);
        while (!waiters_.Empty()) {
            CoroutineMeta* waiter = waiters_.Pop();
            if (waiter) {
                to_wake.push_back(waiter);
            }
        }
    }

    // Wake all outside the lock to avoid holding lock during EnqueueCoroutine
    for (CoroutineMeta* waiter : to_wake) {
        waiter->state.store(CoroutineMeta::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
    }
}

} // namespace coro