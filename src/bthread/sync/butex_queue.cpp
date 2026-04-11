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

// Platform-specific pause instruction for spin loops
#if defined(__x86_64__) || defined(__i386__)
    #define BTHREAD_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
    #define BTHREAD_PAUSE() asm volatile("isb" ::: "memory")
#else
    #define BTHREAD_PAUSE() do {} while(0)  // compiler barrier fallback
#endif

TaskMeta* ButexQueue::PopFromHead() {
    // Spin threshold for empty queue waiting
    constexpr int MAX_EMPTY_SPINS = 1000;
    int empty_spin_count = 0;

    while (true) {
        ButexWaiterNode* head = head_.load(std::memory_order_acquire);

        // Empty queue check
        if (!head) {
            ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
            if (!tail) {
                return nullptr;  // Truly empty - OK to return nullptr
            }

            // tail != null but head == null: node being enqueued
            // Wait for head to be set, DO NOT return nullptr
            if (++empty_spin_count < MAX_EMPTY_SPINS) {
                BTHREAD_PAUSE();
            } else {
                std::this_thread::yield();
                empty_spin_count = 0;  // Reset counter after yield
            }
            continue;  // Keep retrying, never return nullptr here
        }

        // Has nodes, reset empty counter
        empty_spin_count = 0;

        // Try to claim this node first (ABA prevention)
        if (head->claimed.exchange(true, std::memory_order_acq_rel)) {
            // Already claimed by another consumer, try to skip
            ButexWaiterNode* next = head->next.load(std::memory_order_acquire);
            if (next) {
                // Try to advance head past this claimed node
                ButexWaiterNode* expected = head;
                head_.compare_exchange_weak(expected, next,
                    std::memory_order_acq_rel, std::memory_order_relaxed);
            } else {
                // No next, check if queue is truly empty
                ButexWaiterNode* tail = tail_.load(std::memory_order_acquire);
                if (tail == head) {
                    // Queue is empty (head and tail both point to claimed node)
                    ButexWaiterNode* expected_head = head;
                    if (head_.compare_exchange_strong(expected_head, nullptr,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        // Use CAS for tail update to avoid race with AddToTail
                        // which may have set tail to a new node after our head CAS
                        ButexWaiterNode* expected_tail = head;
                        tail_.compare_exchange_strong(expected_tail, nullptr,
                            std::memory_order_acq_rel, std::memory_order_relaxed);
                    }
                }
                // Brief pause before retry
                BTHREAD_PAUSE();
            }
            continue;  // Retry, don't return nullptr
        }

        // Successfully claimed, load next
        ButexWaiterNode* next = head->next.load(std::memory_order_acquire);

        // If next is null, this is the last node - need to update tail too
        if (!next) {
            // Try to advance head and clear tail atomically
            ButexWaiterNode* expected_head = head;
            if (head_.compare_exchange_strong(expected_head, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Head advanced successfully, now clear tail
                // Use CAS to ensure we only clear tail if it still points to our node
                ButexWaiterNode* expected_tail = head;
                tail_.compare_exchange_strong(expected_tail, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed);

                return reinterpret_cast<TaskMeta*>(
                    reinterpret_cast<char*>(head) - offsetof(TaskMeta, butex_waiter_node));
            }
            // CAS failed (rare), release claim and retry
            head->claimed.store(false, std::memory_order_relaxed);
            BTHREAD_PAUSE();
            continue;
        }

        // Try to advance head (there's a next node)
        ButexWaiterNode* expected = head;
        if (head_.compare_exchange_strong(expected, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Successfully claimed and advanced
            return reinterpret_cast<TaskMeta*>(
                reinterpret_cast<char*>(head) - offsetof(TaskMeta, butex_waiter_node));
        }

        // CAS failed (rare), release claim and retry
        head->claimed.store(false, std::memory_order_relaxed);
        BTHREAD_PAUSE();
        // Retry, don't return nullptr
    }
}

int ButexQueue::PopMultipleFromHead(TaskMeta** buffer, int max_count) {
    int count = 0;
    while (count < max_count) {
        TaskMeta* task = PopFromHead();
        if (!task) break;
        buffer[count++] = task;
    }
    return count;
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