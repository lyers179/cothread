#include "bthread/sync/butex_queue.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/platform/platform.h"

#include <thread>

namespace bthread {

void ButexQueue::AddToTail(TaskMeta* task) {
    // Verify task is still supposed to be in queue
    if (!task->is_waiting.load(std::memory_order_relaxed)) {
        return;
    }

    ButexWaiterNode* node = &task->butex_waiter_node;
    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    // Exchange tail - acq_rel provides full barrier
    ButexWaiterNode* prev = tail_.exchange(node, std::memory_order_acq_rel);

    // Check again after exchange - Wake could have cleared is_waiting
    // Use acquire to synchronize with Wake's release store
    if (!task->is_waiting.load(std::memory_order_acquire)) {
        // Mark as claimed so PopFromHead will skip us
        node->claimed.store(true, std::memory_order_release);
        return;
    }

    if (prev) {
        // Link previous node to new node - this makes node visible to PopFromHead
        prev->next.store(node, std::memory_order_release);
    } else {
        // First node - set head. Note: there's a window where tail is set but head isn't
        // PopFromHead handles this by checking tail != head
        ButexWaiterNode* expected = nullptr;
        head_.compare_exchange_strong(expected, node,
            std::memory_order_release, std::memory_order_relaxed);
    }
}

void ButexQueue::AddToHead(TaskMeta* task) {
    if (!task->is_waiting.load(std::memory_order_relaxed)) {
        return;
    }

    ButexWaiterNode* node = &task->butex_waiter_node;
    // Don't set claimed=true - Wait() already initializes it to false

    // Use CAS loop for head insertion
    while (true) {
        ButexWaiterNode* old_head = head_.load(std::memory_order_acquire);

        // Check is_waiting before CAS - Wake could have cleared it
        if (!task->is_waiting.load(std::memory_order_relaxed)) {
            return;
        }

        node->next.store(old_head, std::memory_order_relaxed);

        if (head_.compare_exchange_strong(old_head, node,
                std::memory_order_release, std::memory_order_relaxed)) {
            // CAS succeeded - check is_waiting one more time
            // Use acquire to synchronize with Wake's release store
            if (!task->is_waiting.load(std::memory_order_acquire)) {
                // Wake cleared is_waiting during CAS
                // Check if Wake already set state to READY
                TaskState state = task->state.load(std::memory_order_acquire);
                if (state == TaskState::READY || state == TaskState::RUNNING) {
                    // Wake already consumed this node, no need to rollback
                    // Node is orphaned in queue but marked as claimed below
                    // PopFromHead will skip it when it reaches this node
                    node->claimed.store(true, std::memory_order_release);
                    return;
                }

                // Wake cleared is_waiting but hasn't set state yet
                // Wait() will check is_waiting and state before suspending
                // Mark node as claimed so it will be skipped
                node->claimed.store(true, std::memory_order_release);

                // Roll back head to old_head (best effort)
                // Note: Another thread might have advanced head already
                ButexWaiterNode* expected = node;
                head_.compare_exchange_strong(expected, old_head,
                    std::memory_order_release, std::memory_order_relaxed);

                return;
            }

            // Successfully inserted at head
            // If this was the first node, also update tail
            if (!old_head) {
                ButexWaiterNode* expected = nullptr;
                tail_.compare_exchange_strong(expected, node,
                    std::memory_order_release, std::memory_order_relaxed);
            }
            return;
        }
        // CAS failed, retry
    }
}

TaskMeta* ButexQueue::PopFromHead() {
    int spin_count = 0;
    constexpr int MAX_SPINS = 10000;
    while (true) {
        // Load head with acquire
        ButexWaiterNode* head = head_.load(std::memory_order_acquire);
        if (!head) {
            // Check if tail is non-null - there might be a node being added
            ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
            if (tail && tail != head) {
                // Node being added, spin briefly
                if (spin_count++ < MAX_SPINS) {
                    std::this_thread::yield();
                    continue;
                }
                // Timeout: tail set but head not set after many spins
                // This indicates a stuck state - return nullptr to avoid deadlock
                return nullptr;
            }
            return nullptr;
        }

        // Try to claim this node
        if (head->claimed.exchange(true, std::memory_order_acq_rel)) {
            // Already claimed, try to skip to next
            ButexWaiterNode* next = head->next.load(std::memory_order_acquire);
            if (next) {
                // Try to advance head past this claimed node
                ButexWaiterNode* expected = head;
                head_.compare_exchange_weak(expected, next,
                    std::memory_order_acq_rel, std::memory_order_relaxed);
            } else {
                // No next, and head is claimed - check if queue is truly empty
                ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
                if (tail == head) {
                    // Queue is empty (head and tail both point to claimed node with no next)
                    // Try to reset both to nullptr
                    ButexWaiterNode* expected_head = head;
                    if (head_.compare_exchange_strong(expected_head, nullptr,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        tail_.store(nullptr, std::memory_order_release);
                    }
                }
                if (spin_count++ >= MAX_SPINS) {
                    // Timeout waiting for next or queue reset
                    return nullptr;
                }
            }
            continue;
        }

        // Load next with acquire to synchronize with AddToTail's release store
        ButexWaiterNode* next = head->next.load(std::memory_order_acquire);

        // If next is null but tail != head, a node is being added
        if (!next) {
            ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
            if (tail != head) {
                // Node being added, spin and retry
                head->claimed.store(false, std::memory_order_relaxed);
                if (spin_count++ < MAX_SPINS) {
                    std::this_thread::yield();
                } else {
                    // Timeout: return nullptr to avoid deadlock
                    return nullptr;
                }
                continue;
            }
        }

        // Try to advance head - acq_rel provides synchronization for accessing head->task
        ButexWaiterNode* expected = head;
        if (head_.compare_exchange_strong(expected, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Successfully claimed and advanced
            return reinterpret_cast<TaskMeta*>(
                reinterpret_cast<char*>(head) - offsetof(TaskMeta, butex_waiter_node));
        }

        // CAS failed, reset claimed and retry
        head->claimed.store(false, std::memory_order_relaxed);
        if (spin_count++ >= MAX_SPINS) {
            // Too many retries, return nullptr
            return nullptr;
        }
    }
}

void ButexQueue::RemoveFromWaitQueue(TaskMeta* task) {
    // First mark as not waiting atomically
    bool expected = true;
    if (!task->is_waiting.compare_exchange_strong(expected, false,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;  // Already removed or not in queue
    }

    // Mark node as claimed so PopFromHead will skip it
    task->butex_waiter_node.claimed.store(true, std::memory_order_release);

    // Note: We don't actually remove from linked structure
    // PopFromHead will handle it when it reaches this node
}

} // namespace bthread