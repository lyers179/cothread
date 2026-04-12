#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <thread>

// Platform-specific pause instruction for spin loops
#if defined(__x86_64__) || defined(__i386__)
    #define BTHREAD_MPMC_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
    #define BTHREAD_MPMC_PAUSE() asm volatile("isb" ::: "memory")
#else
    #define BTHREAD_MPMC_PAUSE() do {} while(0)  // compiler barrier fallback
#endif

namespace bthread {

// Forward declarations
struct TaskMeta;

/**
 * @brief Node structure for MpmcQueue.
 *
 * Embed this in your struct to use with MpmcQueue.
 * Example:
 * ```cpp
 * struct MyType {
 *     MpmcNode mpmc_node;  // Embed this
 *     // ... other fields
 * };
 * ```
 */
struct MpmcNode {
    std::atomic<MpmcNode*> next{nullptr};
    std::atomic<bool> claimed{false};  // Prevents double consumption (ABA prevention)
};

// ========== MPMC Field Access Policies ==========

/**
 * @brief Policy for embedded MpmcNode field.
 *
 * Accesses `next` and `claimed` fields via `mpmc_node` sub-object.
 * Use this when MpmcNode is embedded as a member (most common case).
 */
template<typename T>
struct MpmcEmbeddedNodePolicy {
    static std::atomic<T*>& GetNext(T* obj) {
        return reinterpret_cast<std::atomic<T*>&>(obj->mpmc_node.next);
    }

    static void ClearNext(T* obj) {
        obj->mpmc_node.next.store(nullptr, std::memory_order_relaxed);
    }

    static std::atomic<bool>& GetClaimed(T* obj) {
        return obj->mpmc_node.claimed;
    }

    static void ClearClaimed(T* obj) {
        obj->mpmc_node.claimed.store(false, std::memory_order_relaxed);
    }
};

/**
 * @brief Policy for direct next/claimed fields.
 *
 * Use this when the struct has `next` and `claimed` as direct members.
 */
template<typename T>
struct MpmcDirectFieldPolicy {
    static std::atomic<T*>& GetNext(T* obj) {
        return obj->next;
    }

    static void ClearNext(T* obj) {
        obj->next.store(nullptr, std::memory_order_relaxed);
    }

    static std::atomic<bool>& GetClaimed(T* obj) {
        return obj->claimed;
    }

    static void ClearClaimed(T* obj) {
        obj->claimed.store(false, std::memory_order_relaxed);
    }
};

// TaskState enum values for AddToHead rollback logic
enum MpmcTaskStateValue {
    MPMC_TASK_STATE_READY = 1,
    MPMC_TASK_STATE_RUNNING = 2,
};

/**
 * @brief Generic lock-free MPMC (Multi-Producer Multi-Consumer) queue.
 *
 * Uses `claimed` flag for ABA prevention in multi-consumer scenarios.
 *
 * Thread safety:
 * - AddToTail/AddToHead: Safe from multiple producers
 * - PopFromHead: Safe from multiple consumers
 *
 * @tparam T Type with embedded MpmcNode or direct next/claimed fields.
 * @tparam Policy Field access policy (default: MpmcEmbeddedNodePolicy).
 */
template<typename T, typename Policy = MpmcEmbeddedNodePolicy<T>>
class MpmcQueue {
public:
    MpmcQueue() = default;
    ~MpmcQueue() = default;

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    void AddToTail(T* item, std::atomic<bool>* is_waiting = nullptr) {
        if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
            return;
        }

        Policy::ClearNext(item);
        Policy::ClearClaimed(item);

        T* prev = tail_.exchange(item, std::memory_order_acq_rel);

        if (is_waiting && !is_waiting->load(std::memory_order_acquire)) {
            Policy::GetClaimed(item).store(true, std::memory_order_release);
            return;
        }

        if (prev) {
            Policy::GetNext(prev).store(item, std::memory_order_release);
        } else {
            T* expected = nullptr;
            head_.compare_exchange_strong(expected, item,
                std::memory_order_release, std::memory_order_relaxed);
        }
    }

    void AddToHead(T* item, std::atomic<bool>* is_waiting = nullptr,
                   std::atomic<uint8_t>* state = nullptr) {
        if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
            return;
        }

        while (true) {
            T* old_head = head_.load(std::memory_order_acquire);

            if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
                return;
            }

            Policy::GetNext(item).store(old_head, std::memory_order_relaxed);

            if (head_.compare_exchange_strong(old_head, item,
                    std::memory_order_release, std::memory_order_relaxed)) {
                if (is_waiting && !is_waiting->load(std::memory_order_acquire)) {
                    if (state) {
                        uint8_t s = state->load(std::memory_order_acquire);
                        if (s == MPMC_TASK_STATE_READY || s == MPMC_TASK_STATE_RUNNING) {
                            Policy::GetClaimed(item).store(true, std::memory_order_release);
                            return;
                        }
                    }

                    Policy::GetClaimed(item).store(true, std::memory_order_release);

                    T* expected = item;
                    head_.compare_exchange_strong(expected, old_head,
                        std::memory_order_release, std::memory_order_relaxed);

                    return;
                }

                if (!old_head) {
                    T* expected = nullptr;
                    tail_.compare_exchange_strong(expected, item,
                        std::memory_order_release, std::memory_order_relaxed);
                }
                return;
            }
        }
    }

    T* PopFromHead() {
        constexpr int MAX_EMPTY_SPINS = 1000;
        int empty_spin_count = 0;

        while (true) {
            T* head = head_.load(std::memory_order_acquire);

            if (!head) {
                T* tail = tail_.load(std::memory_order_acquire);
                if (!tail) {
                    return nullptr;
                }

                if (++empty_spin_count < MAX_EMPTY_SPINS) {
                    BTHREAD_MPMC_PAUSE();
                } else {
                    std::this_thread::yield();
                    empty_spin_count = 0;
                }
                continue;
            }

            empty_spin_count = 0;

            if (Policy::GetClaimed(head).exchange(true, std::memory_order_acq_rel)) {
                T* next = Policy::GetNext(head).load(std::memory_order_acquire);
                if (next) {
                    T* expected = head;
                    head_.compare_exchange_weak(expected, next,
                        std::memory_order_acq_rel, std::memory_order_relaxed);
                } else {
                    T* tail = tail_.load(std::memory_order_acquire);
                    if (tail == head) {
                        T* expected_head = head;
                        if (head_.compare_exchange_strong(expected_head, nullptr,
                                std::memory_order_acq_rel, std::memory_order_relaxed)) {
                            T* expected_tail = head;
                            tail_.compare_exchange_strong(expected_tail, nullptr,
                                std::memory_order_acq_rel, std::memory_order_relaxed);
                        }
                    }
                    BTHREAD_MPMC_PAUSE();
                }
                continue;
            }

            T* next = Policy::GetNext(head).load(std::memory_order_acquire);

            if (!next) {
                T* expected_head = head;
                if (head_.compare_exchange_strong(expected_head, nullptr,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    T* expected_tail = head;
                    tail_.compare_exchange_strong(expected_tail, nullptr,
                        std::memory_order_acq_rel, std::memory_order_relaxed);

                    return head;
                }
                Policy::GetClaimed(head).store(false, std::memory_order_relaxed);
                BTHREAD_MPMC_PAUSE();
                continue;
            }

            T* expected = head;
            if (head_.compare_exchange_strong(expected, next,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return head;
            }

            Policy::GetClaimed(head).store(false, std::memory_order_relaxed);
            BTHREAD_MPMC_PAUSE();
        }
    }

    int PopMultipleFromHead(T** buffer, int max_count) {
        int count = 0;
        while (count < max_count) {
            T* item = PopFromHead();
            if (!item) break;
            buffer[count++] = item;
        }
        return count;
    }

    void Remove(T* item, std::atomic<bool>* is_waiting) {
        if (!is_waiting) return;

        bool expected = true;
        if (!is_waiting->compare_exchange_strong(expected, false,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }

        Policy::GetClaimed(item).store(true, std::memory_order_release);
    }

    bool IsEmpty() const {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<T*> head_{nullptr};
    std::atomic<T*> tail_{nullptr};
};

// Type alias for Butex waiter queue
using ButexWaiterQueue = MpmcQueue<TaskMeta>;

} // namespace bthread