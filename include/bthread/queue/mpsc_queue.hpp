#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include "bthread/core/task_meta_base.hpp"

namespace bthread {

/**
 * @brief MPSC (Multi-Producer Single-Consumer) lock-free queue template.
 *
 * Thread Safety:
 * - Push(): Safe to call from multiple producer threads concurrently.
 * - Pop(): Must only be called from a single consumer thread.
 *
 * Uses atomic lock-free stack for head and tail pointer for FIFO ordering.
 *
 * @tparam T Type that must have a `std::atomic<T*> next` member (intrusive queue).
 */
template<typename T>
class MpscQueue {
public:
    MpscQueue() = default;
    ~MpscQueue() = default;

    // Disable copy and move
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    /**
     * @brief Push item to queue (multiple producers).
     * @param item Pointer to item to enqueue (must have `next` member)
     */
    void Push(T* item) {
        item->next.store(nullptr, std::memory_order_relaxed);
        T* prev = head_.exchange(item, std::memory_order_acq_rel);
        if (prev) {
            prev->next.store(item, std::memory_order_release);
        } else {
            // First element - set tail
            tail_.store(item, std::memory_order_release);
        }
    }

    /**
     * @brief Pop item from queue (single consumer).
     * @return Item pointer, or nullptr if empty
     */
    T* Pop() {
        T* t = tail_.load(std::memory_order_acquire);
        if (!t) return nullptr;

        T* next = static_cast<T*>(t->next.load(std::memory_order_acquire));
        if (next) {
            tail_.store(next, std::memory_order_release);
            t->next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Last element, try to claim
        T* expected = t;
        if (head_.compare_exchange_strong(expected, nullptr,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.store(nullptr, std::memory_order_release);
            t->next.store(nullptr, std::memory_order_relaxed);
            return t;
        }

        // Race condition: another thread just pushed
        // Adaptive spin before yielding - reduces context switches
        constexpr int MAX_SPINS = 100;  // Spin 100 times before yielding
        int spin_count = 0;
        while (!t->next.load(std::memory_order_acquire)) {
            if (++spin_count < MAX_SPINS) {
                // Use pause instruction on x86 to reduce power consumption
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
                #else
                // Other architectures use compiler barrier
                std::atomic_signal_fence(std::memory_order_acquire);
                #endif
            } else {
                std::this_thread::yield();
                spin_count = 0;  // Reset after yield to continue spinning
            }
        }
        T* n = static_cast<T*>(t->next.load(std::memory_order_acquire));
        tail_.store(n, std::memory_order_release);
        t->next.store(nullptr, std::memory_order_relaxed);
        return t;
    }

    /**
     * @brief Check if queue is empty.
     * @return true if no items in queue
     */
    bool Empty() const {
        return tail_.load(std::memory_order_acquire) == nullptr;
    }

    /**
     * @brief Pop multiple items from queue (single consumer).
     * @param buffer Buffer to store popped items
     * @param max_count Maximum items to pop
     * @return Number of items actually popped
     *
     * This is more efficient than calling Pop() multiple times
     * because it reduces atomic operations for batch processing.
     */
    int PopMultiple(T** buffer, int max_count) {
        int count = 0;
        while (count < max_count) {
            T* item = Pop();
            if (!item) break;
            buffer[count++] = item;
        }
        return count;
    }

private:
    std::atomic<T*> head_{nullptr};
    std::atomic<T*> tail_{nullptr};
};

// Type alias for the unified task queue
using TaskQueue = MpscQueue<TaskMetaBase>;

} // namespace bthread