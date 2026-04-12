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
 *
 * Example:
 * ```cpp
 * struct TaskMeta {
 *     MpmcNode mpmc_node;  // Embedded
 *     ...
 * };
 * using ButexWaiterQueue = MpmcQueue<TaskMeta, MpmcEmbeddedNodePolicy<TaskMeta>>;
 * ```
 */
template<typename T>
struct MpmcEmbeddedNodePolicy {
    // Note: MpmcNode* is cast to T* internally, so we need to access via offset
    // But since we work with T* directly, we access mpmc_node member

    static std::atomic<T*>& GetNext(T* obj) {
        // This is tricky - mpmc_node.next is atomic<MpmcNode*>, not atomic<T*>
        // We need a different approach: store the T* pointer but interpret as MpmcNode*
        // Actually, for embedded node, the next pointer IS the T* pointer
        // We reinterpret_cast<MpmcNode*> to T* for type safety
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
 * Use this when the struct has `next` and `claimed` as direct members
 * (not embedded in a sub-object).
 *
 * Example:
 * ```cpp
 * struct MyType {
 *     std::atomic<MyType*> next{nullptr};
 *     std::atomic<bool> claimed{false};
 *     ...
 * };
 * using MyQueue = MpmcQueue<MyType, MpmcDirectFieldPolicy<MyType>>;
 * ```
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

/**
 * @brief Generic lock-free MPMC (Multi-Producer Multi-Consumer) queue.
 *
 * This class implements a lock-free multiple-producer multiple-consumer queue.
 * Uses `claimed` flag for ABA prevention in multi-consumer scenarios.
 *
 * Thread safety:
 * - AddToTail/AddToHead can be called from multiple threads concurrently (MPMC)
 * - PopFromHead can be called from multiple threads concurrently (MPMC)
 *
 * Race condition handling:
 * - If is_waiting is provided, AddToTail/AddToHead check it after the exchange/CAS
 * - This handles the race where Wake() clears is_waiting during Add operation
 *
 * @tparam T Type that must have MpmcNode embedded or direct next/claimed fields.
 * @tparam Policy Field access policy (default: MpmcEmbeddedNodePolicy).
 */
template<typename T, typename Policy = MpmcEmbeddedNodePolicy<T>>
class MpmcQueue {
public:
    MpmcQueue() = default;
    ~MpmcQueue() = default;

    // Disable copy and move
    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    /**
     * @brief Add an item to the tail of the queue (FIFO order).
     * @param item The item to add.
     * @param is_waiting Optional pointer to is_waiting flag for race handling.
     *                   If provided, checked after exchange to handle Wake() race.
     * @note Caller should initialize item's next/claimed fields before calling.
     */
    void AddToTail(T* item, std::atomic<bool>* is_waiting = nullptr);

    /**
     * @brief Add an item to the head of the queue (LIFO order).
     * @param item The item to add.
     * @param is_waiting Optional pointer to is_waiting flag for race handling.
     * @param state Optional pointer to state for LIFO rollback handling.
     * @note Caller should initialize item's claimed field before calling.
     */
    void AddToHead(T* item, std::atomic<bool>* is_waiting = nullptr,
                   std::atomic<uint8_t>* state = nullptr);

    /**
     * @brief Pop an item from the head of the queue.
     * @return The item at the head, or nullptr if queue is empty.
     *
     * Uses adaptive spinning: pause (CPU instruction) first, then yield.
     */
    T* PopFromHead();

    /**
     * @brief Pop multiple items from the head of the queue.
     * @param buffer Buffer to store popped items.
     * @param max_count Maximum number of items to pop.
     * @return Number of items actually popped.
     */
    int PopMultipleFromHead(T** buffer, int max_count);

    /**
     * @brief Mark an item as claimed (skip during pop) and clear is_waiting.
     * @param item The item to mark.
     * @param is_waiting Pointer to is_waiting flag to clear.
     */
    void Remove(T* item, std::atomic<bool>* is_waiting);

    /**
     * @brief Check if the queue is empty.
     * @return true if the queue appears empty, false otherwise.
     */
    bool IsEmpty() const {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<T*> head_{nullptr};
    std::atomic<T*> tail_{nullptr};
};

// Type alias for Butex waiter queue (uses embedded MpmcNode in TaskMeta)
using ButexWaiterQueue = MpmcQueue<TaskMeta>;

} // namespace bthread