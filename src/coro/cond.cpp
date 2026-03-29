// src/coro/cond.cpp
#include "coro/cond.h"
#include "coro/scheduler.h"
#include <cassert>
#include <stdexcept>
#include <thread>

namespace coro {

CoCond::CoCond() = default;

CoCond::~CoCond() {
    // Critical: Destroying condition variable with pending waiters leaks coroutines
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

    // Add to waiters queue
    cond_.waiters_.Push(meta_);

    return true;  // Suspend
}

void CoCond::WaitAwaiter::await_resume() {
    // Called when coroutine resumes - need to re-acquire mutex
    // The mutex was unlocked in await_suspend, and we must re-acquire it
    // before returning from co_await.

    // TODO(#future): The current implementation uses a spin-yield loop for mutex
    // re-acquisition. This is a limitation because await_resume() cannot be a
    // coroutine and cannot co_await. A proper solution would require:
    //   1. Storing the LockAwaiter state in WaitAwaiter and using a two-phase
    //      suspension mechanism, or
    //   2. Using an atomic state machine to track lock acquisition state.
    // This spin-yield is acceptable for now because:
    //   - Condition variable semantics require the lock holder to release quickly
    //   - The scheduler ensures fair scheduling of contending coroutines
    //   - The yield() prevents busy-waiting from consuming excessive CPU
    while (!mutex_.try_lock()) {
        std::this_thread::yield();
    }
    // Mutex is now held by us
}

void CoCond::signal() {
    CoroutineMeta* waiter = waiters_.Pop();
    if (waiter) {
        waiter->state.store(CoroutineMeta::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
    }
}

void CoCond::broadcast() {
    while (!waiters_.Empty()) {
        CoroutineMeta* waiter = waiters_.Pop();
        if (waiter) {
            waiter->state.store(CoroutineMeta::READY, std::memory_order_release);
            waiter->waiting_sync = nullptr;
            CoroutineScheduler::Instance().EnqueueCoroutine(waiter);
        }
    }
}

} // namespace coro