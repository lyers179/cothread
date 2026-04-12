#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace bthread {

// Forward declarations
struct TaskMeta;

/**
 * @brief Node structure for MpmcQueue.
 *
 * Embed this in your struct to use with MpmcQueue.
 */
struct MpmcNode {
    std::atomic<MpmcNode*> next{nullptr};
    std::atomic<bool> claimed{false};  // Prevents double consumption
};

/**
 * @brief Generic lock-free MPMC (Multi-Producer Multi-Consumer) queue.
 *
 * This class implements a lock-free multiple-producer multiple-consumer queue.
 * It operates on MpmcNode pointers embedded in parent structures.
 *
 * The queue supports:
 * - AddToTail: FIFO ordering (multiple producers)
 * - AddToHead: LIFO ordering (for re-queueing)
 * - PopFromHead: MPMC consumer operation (CAS retry)
 *
 * Thread safety:
 * - AddToTail/AddToHead can be called from multiple threads concurrently (MPMC)
 * - PopFromHead can be called from multiple threads concurrently (MPMC)
 *
 * Race condition handling:
 * - If is_waiting is provided, AddToTail/AddToHead check it after the exchange/CAS
 * - This handles the race where Wake() clears is_waiting during Add operation
 */
class MpmcQueue {
public:
    MpmcQueue() = default;
    ~MpmcQueue() = default;

    // Disable copy and move
    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    /**
     * @brief Add a node to the tail of the queue (FIFO order).
     * @param node The node to add.
     * @param is_waiting Optional pointer to is_waiting flag for race handling.
     *                   If provided, checked after exchange to handle Wake() race.
     * @note Caller should initialize node->next=nullptr, node->claimed=false before calling.
     */
    void AddToTail(MpmcNode* node, std::atomic<bool>* is_waiting = nullptr);

    /**
     * @brief Add a node to the head of the queue (LIFO order).
     * @param node The node to add.
     * @param is_waiting Optional pointer to is_waiting flag for race handling.
     * @param state Optional pointer to state for LIFO rollback handling (uint8_t for enum class).
     * @note Caller should initialize node->claimed=false before calling.
     */
    void AddToHead(MpmcNode* node, std::atomic<bool>* is_waiting = nullptr,
                   std::atomic<uint8_t>* state = nullptr);

    /**
     * @brief Pop a node from the head of the queue.
     * @return The node at the head, or nullptr if queue is empty.
     *
     * Uses adaptive spinning: pause (CPU instruction) first, then yield.
     */
    MpmcNode* PopFromHead();

    /**
     * @brief Pop multiple nodes from the head of the queue.
     * @param buffer Buffer to store popped nodes.
     * @param max_count Maximum number of nodes to pop.
     * @return Number of nodes actually popped.
     */
    int PopMultipleFromHead(MpmcNode** buffer, int max_count);

    /**
     * @brief Mark a node as claimed (skip during pop) and clear is_waiting.
     * @param node The node to mark.
     * @param is_waiting Pointer to is_waiting flag to clear.
     */
    void Remove(MpmcNode* node, std::atomic<bool>* is_waiting);

    /**
     * @brief Check if the queue is empty.
     * @return true if the queue appears empty, false otherwise.
     */
    bool IsEmpty() const {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<MpmcNode*> head_{nullptr};
    std::atomic<MpmcNode*> tail_{nullptr};
};

// Helper: Convert MpmcNode* to parent pointer using offset
template <typename Parent>
Parent* NodeToParent(MpmcNode* node, size_t offset) {
    if (!node) return nullptr;
    return reinterpret_cast<Parent*>(reinterpret_cast<char*>(node) - offset);
}

// Helper: Convert parent pointer to MpmcNode* using offset
template <typename Parent>
MpmcNode* ParentToNode(Parent* parent, size_t offset) {
    if (!parent) return nullptr;
    return reinterpret_cast<MpmcNode*>(reinterpret_cast<char*>(parent) + offset);
}

// Type alias for backward compatibility
using ButexWaiterQueue = MpmcQueue;

} // namespace bthread