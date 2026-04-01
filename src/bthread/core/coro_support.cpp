// src/bthread/core/coro_support.cpp
// Provides coroutine support for the bthread library

#include "coro/coroutine.h"

namespace coro {

// Define the thread-local variable here to ensure it's available for linking
// This is needed because the bthread library uses current_coro_meta() in sync primitives
thread_local CoroutineMeta* current_coro_meta_ = nullptr;

// Implement the function to return the current coroutine meta
CoroutineMeta* current_coro_meta() {
    return current_coro_meta_;
}

} // namespace coro