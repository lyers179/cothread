// include/coro/meta.h
#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <thread>

namespace bthread {
    class Worker;
}

namespace coro {

/**
 * @brief Coroutine metadata - manages coroutine lifecycle.
 *
 * Thread Safety Guarantees:
 * - `state`: Atomic, safe for concurrent state transitions.
 * - `cancel_requested`: Atomic, safe for concurrent cancellation checks.
 * - `next`: Atomic, used for intrusive MPSC queue linkage.
 * - `handle`, `owner_worker`, `waiting_sync`, `slot_index`, `generation`:
 *   Not thread-safe. These are owned by a single worker thread at any time.
 */
struct CoroutineMeta {
    enum State : uint8_t {
        READY,      ///< Coroutine is ready to be scheduled
        RUNNING,    ///< Coroutine is currently executing
        SUSPENDED,  ///< Coroutine is waiting on a synchronization primitive
        FINISHED    ///< Coroutine has completed execution
    };

    std::coroutine_handle<> handle;
    std::atomic<State> state{READY};  ///< Atomic for cross-thread state transitions
    bthread::Worker* owner_worker{nullptr};
    std::atomic<bool> cancel_requested{false};
    void* waiting_sync{nullptr};  ///< CoMutex/CoCond pointer if waiting

    // Intrusive queue linkage (atomic for MPSC queue safety)
    std::atomic<CoroutineMeta*> next{nullptr};

    // Identification
    uint32_t slot_index{0};
    uint32_t generation{0};
};

/**
 * @brief Intrusive MPSC (Multi-Producer Single-Consumer) queue for coroutines.
 *
 * Thread Safety:
 * - Push(): Safe to call from multiple producer threads concurrently.
 * - Pop(): Must only be called from a single consumer thread.
 *
 * This queue uses an atomic lock-free stack for the head and a tail pointer
 * for FIFO ordering. Producers push to the head, the consumer pops from tail.
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

        CoroutineMeta* next = t->next.load(std::memory_order_acquire);
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
        CoroutineMeta* n = t->next.load(std::memory_order_acquire);
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