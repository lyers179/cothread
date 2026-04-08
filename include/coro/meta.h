// include/coro/meta.h
#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <thread>

#include "bthread/queue/mpsc_queue.hpp"

namespace coro {

/**
 * @brief Coroutine metadata - manages coroutine lifecycle.
 *
 * Inherits from bthread::TaskMetaBase and adds coroutine-specific fields.
 *
 * Thread Safety Guarantees:
 * - Inherited atomic fields from TaskMetaBase (state, next)
 * - `handle`: Not thread-safe. Owned by a single worker thread at any time.
 * - `cancel_requested`: Atomic, safe for concurrent cancellation checks.
 */
struct CoroutineMeta : bthread::TaskMetaBase {
    // Constructor - set type to COROUTINE
    CoroutineMeta() : bthread::TaskMetaBase() {
        type = bthread::TaskType::COROUTINE;
    }

    // ========== Legacy State Enum (for backward compatibility) ==========
    // These map to TaskState from TaskMetaBase
    enum State : uint8_t {
        READY = static_cast<uint8_t>(bthread::TaskState::READY),
        RUNNING = static_cast<uint8_t>(bthread::TaskState::RUNNING),
        SUSPENDED = static_cast<uint8_t>(bthread::TaskState::SUSPENDED),
        FINISHED = static_cast<uint8_t>(bthread::TaskState::FINISHED)
    };

    // ========== Coroutine Handle (coroutine-specific) ==========
    std::coroutine_handle<> handle;

    // ========== Cancellation (coroutine-specific) ==========
    std::atomic<bool> cancel_requested{false};

    // ========== Resume Implementation ==========
    void resume() override {
        if (handle && !handle.done()) {
            handle.resume();
        }
    }
};

// Get current coroutine's meta (returns nullptr if not in coroutine)
CoroutineMeta* current_coro_meta();

/**
 * @brief Type alias for coroutine queue - uses unified MPSC queue.
 *
 * Note: CoroutineMeta inherits from TaskMetaBase, so bthread::TaskQueue
 * works directly with CoroutineMeta pointers.
 */
using CoroutineQueue = bthread::TaskQueue;

} // namespace coro