#include "bthread/queue/mpmc_queue.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/platform/platform.h"

#include <thread>

namespace bthread {

// TaskState enum values for AddToHead rollback logic
// These are defined in task_meta_base.hpp
enum TaskStateValue {
    TASK_STATE_READY = 1,
    TASK_STATE_RUNNING = 2,
};

void MpmcQueue::AddToTail(MpmcNode* node, std::atomic<bool>* is_waiting) {
    // If is_waiting is provided, verify task is still supposed to be in queue
    if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
        return;
    }

    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    // Exchange tail - acq_rel provides full barrier
    MpmcNode* prev = tail_.exchange(node, std::memory_order_acq_rel);

    // If is_waiting provided, check again after exchange
    // Wake could have cleared is_waiting during the exchange
    if (is_waiting && !is_waiting->load(std::memory_order_acquire)) {
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
        MpmcNode* expected = nullptr;
        head_.compare_exchange_strong(expected, node,
            std::memory_order_release, std::memory_order_relaxed);
    }
}

void MpmcQueue::AddToHead(MpmcNode* node, std::atomic<bool>* is_waiting,
                          std::atomic<uint8_t>* state) {
    if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
        return;
    }

    // Use CAS loop for head insertion
    while (true) {
        MpmcNode* old_head = head_.load(std::memory_order_acquire);

        // Check is_waiting before CAS - Wake could have cleared it
        if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
            return;
        }

        node->next.store(old_head, std::memory_order_relaxed);

        if (head_.compare_exchange_strong(old_head, node,
                std::memory_order_release, std::memory_order_relaxed)) {
            // CAS succeeded - check is_waiting one more time
            // Use acquire to synchronize with Wake's release store
            if (is_waiting && !is_waiting->load(std::memory_order_acquire)) {
                // Wake cleared is_waiting during CAS
                // Check if Wake already set state to READY
                if (state) {
                    uint8_t s = state->load(std::memory_order_acquire);
                    if (s == TASK_STATE_READY || s == TASK_STATE_RUNNING) {
                        // Wake already consumed this node, no need to rollback
                        // Node is orphaned in queue but marked as claimed below
                        // PopFromHead will skip it when it reaches this node
                        node->claimed.store(true, std::memory_order_release);
                        return;
                    }
                }

                // Wake cleared is_waiting but hasn't set state yet
                // Mark node as claimed so it will be skipped
                node->claimed.store(true, std::memory_order_release);

                // Roll back head to old_head (best effort)
                // Note: Another thread might have advanced head already
                MpmcNode* expected = node;
                head_.compare_exchange_strong(expected, old_head,
                    std::memory_order_release, std::memory_order_relaxed);

                return;
            }

            // Successfully inserted at head
            // If this was the first node, also update tail
            if (!old_head) {
                MpmcNode* expected = nullptr;
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

MpmcNode* MpmcQueue::PopFromHead() {
    // Spin threshold for empty queue waiting
    constexpr int MAX_EMPTY_SPINS = 1000;
    int empty_spin_count = 0;

    while (true) {
        MpmcNode* head = head_.load(std::memory_order_acquire);

        // Empty queue check
        if (!head) {
            MpmcNode* tail = tail_.load(std::memory_order_acquire);
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
            MpmcNode* next = head->next.load(std::memory_order_acquire);
            if (next) {
                // Try to advance head past this claimed node
                MpmcNode* expected = head;
                head_.compare_exchange_weak(expected, next,
                    std::memory_order_acq_rel, std::memory_order_relaxed);
            } else {
                // No next, check if queue is truly empty
                MpmcNode* tail = tail_.load(std::memory_order_acquire);
                if (tail == head) {
                    // Queue is empty (head and tail both point to claimed node)
                    MpmcNode* expected_head = head;
                    if (head_.compare_exchange_strong(expected_head, nullptr,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        // Use CAS for tail update to avoid race with AddToTail
                        // which may have set tail to a new node after our head CAS
                        MpmcNode* expected_tail = head;
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
        MpmcNode* next = head->next.load(std::memory_order_acquire);

        // If next is null, this is the last node - need to update tail too
        if (!next) {
            // Try to advance head and clear tail atomically
            MpmcNode* expected_head = head;
            if (head_.compare_exchange_strong(expected_head, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Head advanced successfully, now clear tail
                // Use CAS to ensure we only clear tail if it still points to our node
                MpmcNode* expected_tail = head;
                tail_.compare_exchange_strong(expected_tail, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed);

                return head;
            }
            // CAS failed (rare), release claim and retry
            head->claimed.store(false, std::memory_order_relaxed);
            BTHREAD_PAUSE();
            continue;
        }

        // Try to advance head (there's a next node)
        MpmcNode* expected = head;
        if (head_.compare_exchange_strong(expected, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Successfully claimed and advanced
            return head;
        }

        // CAS failed (rare), release claim and retry
        head->claimed.store(false, std::memory_order_relaxed);
        BTHREAD_PAUSE();
        // Retry, don't return nullptr
    }
}

int MpmcQueue::PopMultipleFromHead(MpmcNode** buffer, int max_count) {
    int count = 0;
    while (count < max_count) {
        MpmcNode* node = PopFromHead();
        if (!node) break;
        buffer[count++] = node;
    }
    return count;
}

void MpmcQueue::Remove(MpmcNode* node, std::atomic<bool>* is_waiting) {
    if (!is_waiting) return;

    // First mark as not waiting atomically
    bool expected = true;
    if (!is_waiting->compare_exchange_strong(expected, false,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;  // Already removed or not in queue
    }

    // Mark node as claimed so PopFromHead will skip it
    node->claimed.store(true, std::memory_order_release);

    // Note: We don't actually remove from linked structure
    // PopFromHead will handle it when it reaches this node
}

} // namespace bthread