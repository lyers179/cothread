// include/coro/meta.h
#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <thread>

#include "bthread/core/task_meta_base.hpp"

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
 * @brief Intrusive MPSC (Multi-Producer Single-Consumer) queue for coroutines.
 *
 * Thread Safety:
 * - Push(): Safe to call from multiple producer threads concurrently.
 * - Pop(): Must only be called from a single consumer thread.
 *
 * This queue uses an atomic lock-free stack for the head and a tail pointer
 * for FIFO ordering. Producers push to the head, the consumer pops from tail.
 *
 * Note: This queue operates on CoroutineMeta*, which inherits from TaskMetaBase.
 * For a unified queue that works with both task types, use bthread::GlobalQueue.
 */
class CoroutineQueue {
public:
    void Push(CoroutineMeta* meta) {
        meta->next.store(nullptr, std::memory_order_relaxed);
        CoroutineMeta* prev = head_.exchange(meta, std::memory_order_acq_rel);
        if (prev) {
            prev->next.store(meta, std::memory_order_release);
        } else {
            // First element - set tail to the new element
            tail_.store(meta, std::memory_order_release);
        }
    }

    CoroutineMeta* Pop() {
        CoroutineMeta* t = tail_.load(std::memory_order_acquire);
        if (!t) return nullptr;

        CoroutineMeta* next = static_cast<CoroutineMeta*>(t->next.load(std::memory_order_acquire));
        if (next) {
            tail_.store(next, std::memory_order_release);
            t->next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Last element, try to claim
        CoroutineMeta* expected = t;
        if (head_.compare_exchange_strong(expected, nullptr,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.store(nullptr, std::memory_order_release);
            t->next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Race condition: another thread just pushed
        // Wait for next pointer to be set with backoff to avoid spinning
        while (!t->next.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        CoroutineMeta* n = static_cast<CoroutineMeta*>(t->next.load(std::memory_order_acquire));
        tail_.store(n, std::memory_order_release);
        t->next.store(nullptr, std::memory_order_relaxed);
        return t;
    }

    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<CoroutineMeta*> head_{nullptr};
    std::atomic<CoroutineMeta*> tail_{nullptr};
};

} // namespace coro