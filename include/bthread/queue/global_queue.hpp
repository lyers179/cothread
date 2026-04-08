// include/bthread/queue/global_queue.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "bthread/core/task_meta_base.hpp"

namespace bthread {

/**
 * @brief MPSC (Multi-Producer Single-Consumer) queue for global task distribution.
 *
 * Thread Safety:
 * - Push(): Safe to call from multiple producer threads concurrently.
 * - Pop(): Must only be called from a single consumer thread.
 *
 * This queue uses an atomic lock-free stack for the head and a tail pointer
 * for FIFO ordering. Producers push to the head, the consumer pops from tail.
 *
 * Unified Design: Works with both TaskMeta (bthread) and CoroutineMeta (coroutine)
 * through the TaskMetaBase interface.
 */
class GlobalQueue {
public:
    GlobalQueue() = default;
    ~GlobalQueue() = default;

    // Disable copy and move
    GlobalQueue(const GlobalQueue&) = delete;
    GlobalQueue& operator=(const GlobalQueue&) = delete;

    /**
     * @brief Push task to queue (multiple producers).
     * Works with any TaskMetaBase-derived type (TaskMeta, CoroutineMeta).
     * @param task The task to enqueue
     */
    void Push(TaskMetaBase* task) {
        task->next.store(nullptr, std::memory_order_relaxed);
        TaskMetaBase* prev = head_.exchange(task, std::memory_order_acq_rel);
        if (prev) {
            prev->next.store(task, std::memory_order_release);
        } else {
            // First element - set tail to the new element
            tail_.store(task, std::memory_order_release);
        }
    }

    /**
     * @brief Pop all tasks and return as linked list (single consumer).
     * Returns head of reversed list (first to execute).
     * The caller is responsible for iterating through the linked list via next pointers.
     * @return Head of task list, or nullptr if empty
     */
    TaskMetaBase* Pop() {
        TaskMetaBase* t = tail_.load(std::memory_order_acquire);
        if (!t) return nullptr;

        TaskMetaBase* next = t->next.load(std::memory_order_acquire);
        if (next) {
            tail_.store(next, std::memory_order_release);
            t->next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Last element, try to claim
        TaskMetaBase* expected = t;
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
        TaskMetaBase* n = t->next.load(std::memory_order_acquire);
        tail_.store(n, std::memory_order_release);
        t->next.store(nullptr, std::memory_order_relaxed);
        return t;
    }

    /**
     * @brief Check if queue is empty.
     * @return true if no tasks in queue
     */
    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<TaskMetaBase*> head_{nullptr};
    std::atomic<TaskMetaBase*> tail_{nullptr};
    std::atomic<uint64_t> version_{0};  ///< For potential ABA prevention (future use)
};

} // namespace bthread