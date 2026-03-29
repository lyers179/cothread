// src/coro/coroutine.cpp
#include "coro/coroutine.h"
#include "coro/scheduler.h"

// Most implementation is in headers (templates)
// This file is for any non-template definitions

namespace coro {

// YieldAwaiter::await_suspend implementation
bool YieldAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    // Get CoroutineMeta from thread-local (set by CoroutineWorkerLoop)
    CoroutineMeta* meta = current_coro_meta();
    if (meta) {
        // Set state to READY and re-queue for execution
        meta->state.store(CoroutineMeta::READY, std::memory_order_release);
        CoroutineScheduler::Instance().EnqueueCoroutine(meta);
        return true;  // Suspend the coroutine
    }
    // Not in scheduler context, just resume immediately without suspending
    return false;
}

} // namespace coro