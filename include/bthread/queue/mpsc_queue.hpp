#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include "bthread/core/task_meta_base.hpp"

// Platform-specific pause instruction for spin loops
#if defined(__x86_64__) || defined(__i386__)
    #define BTHREAD_QUEUE_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
    #define BTHREAD_QUEUE_PAUSE() asm volatile("isb" ::: "memory")
#else
    #define BTHREAD_QUEUE_PAUSE() do {} while(0)  // compiler barrier fallback
#endif

namespace bthread {

// ========== Next Field Access Policies ==========

/**
 * @brief Default policy: access using `next` field.
 * Used for general-purpose MPSC queues (scheduler, execution queue, etc.)
 */
template<typename T>
struct NextFieldPolicy {
    static std::atomic<T*>& Get(T* obj) { return obj->next; }
    static void Clear(T* obj) {
        obj->next.store(nullptr, std::memory_order_relaxed);
    }
};

/**
 * @brief Waiter policy: access using `waiter_next` field.
 * Used for sync primitive waiter queues (Mutex, CondVar, Event).
 * Mutually exclusive with scheduler's `next` field usage.
 */
template<typename T>
struct WaiterNextFieldPolicy {
    static std::atomic<T*>& Get(T* obj) { return obj->waiter_next; }
    static void Clear(T* obj) {
        obj->waiter_next.store(nullptr, std::memory_order_relaxed);
    }
};

/**
 * @brief MPSC (Multi-Producer Single-Consumer) lock-free queue template.
 *
 * Thread Safety:
 * - Push(): Safe to call from multiple producer threads concurrently.
 * - Pop(): Must only be called from a single consumer thread.
 *
 * Uses atomic lock-free stack for head and tail pointer for FIFO ordering.
 * Uses adaptive spinning: pause first (low latency), then yield.
 *
 * @tparam T Type that must have a `std::atomic<T*> next` member (intrusive queue).
 * @tparam Policy Field access policy (default: NextFieldPolicy for `next` field).
 */
template<typename T, typename Policy = NextFieldPolicy<T>>
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
        Policy::Clear(item);
        T* prev = head_.exchange(item, std::memory_order_acq_rel);
        if (prev) {
            Policy::Get(prev).store(item, std::memory_order_release);
        } else {
            // First element - set tail
            tail_.store(item, std::memory_order_release);
        }
    }

    /**
     * @brief Pop item from queue (single consumer).
     * @return Item pointer, or nullptr if empty
     *
     * Uses adaptive spinning: pause (CPU instruction) first, then yield.
     */
    T* Pop() {
        // Adaptive spinning thresholds
        constexpr int MAX_PAUSE_SPINS = 100;   // Phase 1: CPU pause (~3-10 us)
        constexpr int MAX_YIELD_SPINS = 10;    // Phase 2: yield (~1-10 ms)

        T* t = tail_.load(std::memory_order_acquire);
        if (!t) return nullptr;

        T* next = static_cast<T*>(Policy::Get(t).load(std::memory_order_acquire));
        if (next) {
            tail_.store(next, std::memory_order_release);
            Policy::Clear(t);  // Clear next pointer
            return t;
        }

        // Last element, try to claim
        T* expected = t;
        if (head_.compare_exchange_strong(expected, nullptr,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.store(nullptr, std::memory_order_release);
            Policy::Clear(t);  // Clear next pointer
            return t;
        }

        // Race condition: another thread just pushed
        // Use adaptive spin instead of immediate yield
        int pause_count = 0;
        int yield_count = 0;

        while (true) {
            // Re-load next with acquire (not seq_cst - sufficient for this case)
            T* n = static_cast<T*>(Policy::Get(t).load(std::memory_order_acquire));
            if (n) {
                tail_.store(n, std::memory_order_release);
                Policy::Clear(t);  // Clear next pointer
                return t;
            }

            // Adaptive spin
            if (pause_count < MAX_PAUSE_SPINS) {
                BTHREAD_QUEUE_PAUSE();
                ++pause_count;
                continue;
            }

            if (yield_count < MAX_YIELD_SPINS) {
                std::this_thread::yield();
                ++yield_count;
                pause_count = 0;  // Reset pause after yield
                continue;
            }

            // Timeout after both phases - re-check queue state
            T* current_tail = tail_.load(std::memory_order_acquire);
            if (current_tail != t) {
                // tail changed, restart Pop
                return Pop();
            }
            T* current_head = head_.load(std::memory_order_acquire);
            if (current_head == nullptr) {
                // Queue appears empty now
                return nullptr;
            }
            // Reset counters and continue (rare edge case)
            pause_count = 0;
            yield_count = 0;
        }
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

// Default task queue (uses `next` field)
using TaskQueue = MpscQueue<TaskMetaBase>;

// Waiter queue for sync primitives (uses `waiter_next` field)
using WaiterQueue = MpscQueue<TaskMetaBase, WaiterNextFieldPolicy<TaskMetaBase>>;

} // namespace bthread