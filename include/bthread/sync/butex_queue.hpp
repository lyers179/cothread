#pragma once

#include <atomic>
#include <cstdint>

namespace bthread {

// Forward declarations
struct TaskMeta;
struct ButexWaiterNode;

/**
 * @brief Lock-free MPSC queue for butex waiters.
 *
 * This class implements a lock-free multiple-producer single-consumer queue
 * for managing waiting tasks in synchronization primitives.
 *
 * The queue supports:
 * - AddToTail: FIFO ordering (multiple producers)
 * - AddToHead: LIFO ordering (for re-queueing)
 * - PopFromHead: Consumer operation
 *
 * Thread safety:
 * - AddToTail/AddToHead can be called from multiple threads concurrently
 * - PopFromHead must be called from a single thread (or with external synchronization)
 */
class ButexQueue {
public:
    ButexQueue() = default;
    ~ButexQueue() = default;

    // Disable copy and move
    ButexQueue(const ButexQueue&) = delete;
    ButexQueue& operator=(const ButexQueue&) = delete;

    /**
     * @brief Add a task to the tail of the queue (FIFO order).
     * @param task The task to add. Must have is_waiting set to true.
     */
    void AddToTail(TaskMeta* task);

    /**
     * @brief Add a task to the head of the queue (LIFO order).
     * @param task The task to add. Must have is_waiting set to true.
     */
    void AddToHead(TaskMeta* task);

    /**
     * @brief Pop a task from the head of the queue.
     * @return The task at the head, or nullptr if queue is empty.
     */
    TaskMeta* PopFromHead();

    /**
     * @brief Mark a task as removed from the queue.
     *
     * This doesn't physically remove the task from the linked list,
     * but marks it so PopFromHead will skip it.
     *
     * @param task The task to remove.
     */
    void RemoveFromWaitQueue(TaskMeta* task);

    /**
     * @brief Check if the queue is empty.
     * @return true if the queue appears empty, false otherwise.
     */
    bool IsEmpty() const {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<ButexWaiterNode*> head_{nullptr};
    std::atomic<ButexWaiterNode*> tail_{nullptr};
};

} // namespace bthread