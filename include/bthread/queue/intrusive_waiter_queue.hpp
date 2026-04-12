#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include "bthread/core/task_meta_base.hpp"

// Platform-specific pause instruction for spin loops
#if defined(__x86_64__) || defined(__i386__)
    #define BTHREAD_WAITER_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
    #define BTHREAD_WAITER_PAUSE() asm volatile("isb" ::: "memory")
#else
    #define BTHREAD_WAITER_PAUSE() do {} while(0)
#endif

namespace bthread {

/**
 * @brief Lock-free MPSC queue for TaskMetaBase using intrusive waiter_next linkage.
 *
 * This is a specialized variant of MpscQueue that operates directly on TaskMetaBase
 * using the `waiter_next` field instead of requiring a separate wrapper node.
 *
 * Thread Safety:
 * - Push(): Safe from multiple producers (lock-free MPSC)
 * - Pop(): Single consumer only
 *
 * Usage:
 * Used by sync primitives (Mutex, CondVar, Event) for waiter queues.
 * Eliminates dynamic memory allocation per waiter.
 */
class IntrusiveWaiterQueue {
public:
    IntrusiveWaiterQueue() = default;
    ~IntrusiveWaiterQueue() = default;

    // Disable copy and move
    IntrusiveWaiterQueue(const IntrusiveWaiterQueue&) = delete;
    IntrusiveWaiterQueue& operator=(const IntrusiveWaiterQueue&) = delete;

    /**
     * @brief Push a task to the waiter queue (multiple producers).
     * @param task Task to enqueue (will use task->waiter_next for linkage)
     */
    void Push(TaskMetaBase* task) {
        task->waiter_next.store(nullptr, std::memory_order_relaxed);
        TaskMetaBase* prev = head_.exchange(task, std::memory_order_acq_rel);
        if (prev) {
            prev->waiter_next.store(task, std::memory_order_release);
        } else {
            // First element - set tail
            tail_.store(task, std::memory_order_release);
        }
    }

    /**
     * @brief Pop a task from the waiter queue (single consumer).
     * @return Task pointer, or nullptr if empty
     */
    TaskMetaBase* Pop() {
        // Adaptive spinning thresholds
        constexpr int MAX_PAUSE_SPINS = 100;
        constexpr int MAX_YIELD_SPINS = 10;

        TaskMetaBase* t = tail_.load(std::memory_order_acquire);
        if (!t) return nullptr;

        TaskMetaBase* next = t->waiter_next.load(std::memory_order_acquire);
        if (next) {
            tail_.store(next, std::memory_order_release);
            t->waiter_next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Last element, try to claim
        TaskMetaBase* expected = t;
        if (head_.compare_exchange_strong(expected, nullptr,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.store(nullptr, std::memory_order_release);
            t->waiter_next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Race condition: another thread just pushed
        int pause_count = 0;
        int yield_count = 0;

        while (true) {
            TaskMetaBase* n = t->waiter_next.load(std::memory_order_acquire);
            if (n) {
                tail_.store(n, std::memory_order_release);
                t->waiter_next.store(nullptr, std::memory_order_relaxed);
                return t;
            }

            if (pause_count < MAX_PAUSE_SPINS) {
                BTHREAD_WAITER_PAUSE();
                ++pause_count;
                continue;
            }

            if (yield_count < MAX_YIELD_SPINS) {
                std::this_thread::yield();
                ++yield_count;
                pause_count = 0;
                continue;
            }

            // Timeout - re-check queue state
            TaskMetaBase* current_tail = tail_.load(std::memory_order_acquire);
            if (current_tail != t) {
                return Pop();  // tail changed, restart
            }
            TaskMetaBase* current_head = head_.load(std::memory_order_acquire);
            if (current_head == nullptr) {
                return nullptr;
            }
            pause_count = 0;
            yield_count = 0;
        }
    }

    /**
     * @brief Check if queue is empty.
     */
    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<TaskMetaBase*> head_{nullptr};
    std::atomic<TaskMetaBase*> tail_{nullptr};
};

} // namespace bthread