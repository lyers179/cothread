#include "bthread/queue/mpmc_queue.hpp"
#include "bthread/core/task_meta.hpp"

#include <thread>

namespace bthread {

// TaskState enum values for AddToHead rollback logic
enum TaskStateValue {
    TASK_STATE_READY = 1,
    TASK_STATE_RUNNING = 2,
};

template<typename T, typename Policy>
void MpmcQueue<T, Policy>::AddToTail(T* item, std::atomic<bool>* is_waiting) {
    // If is_waiting is provided, verify task is still supposed to be in queue
    if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
        return;
    }

    Policy::ClearNext(item);
    Policy::ClearClaimed(item);

    // Exchange tail - acq_rel provides full barrier
    T* prev = tail_.exchange(item, std::memory_order_acq_rel);

    // If is_waiting provided, check again after exchange
    // Wake could have cleared is_waiting during the exchange
    if (is_waiting && !is_waiting->load(std::memory_order_acquire)) {
        // Mark as claimed so PopFromHead will skip us
        Policy::GetClaimed(item).store(true, std::memory_order_release);
        return;
    }

    if (prev) {
        // Link previous item to new item - this makes item visible to PopFromHead
        Policy::GetNext(prev).store(item, std::memory_order_release);
    } else {
        // First item - set head. Note: there's a window where tail is set but head isn't
        // PopFromHead handles this by checking tail != head
        T* expected = nullptr;
        head_.compare_exchange_strong(expected, item,
            std::memory_order_release, std::memory_order_relaxed);
    }
}

template<typename T, typename Policy>
void MpmcQueue<T, Policy>::AddToHead(T* item, std::atomic<bool>* is_waiting,
                                      std::atomic<uint8_t>* state) {
    if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
        return;
    }

    // Use CAS loop for head insertion
    while (true) {
        T* old_head = head_.load(std::memory_order_acquire);

        // Check is_waiting before CAS - Wake could have cleared it
        if (is_waiting && !is_waiting->load(std::memory_order_relaxed)) {
            return;
        }

        Policy::GetNext(item).store(old_head, std::memory_order_relaxed);

        if (head_.compare_exchange_strong(old_head, item,
                std::memory_order_release, std::memory_order_relaxed)) {
            // CAS succeeded - check is_waiting one more time
            // Use acquire to synchronize with Wake's release store
            if (is_waiting && !is_waiting->load(std::memory_order_acquire)) {
                // Wake cleared is_waiting during CAS
                // Check if Wake already set state to READY
                if (state) {
                    uint8_t s = state->load(std::memory_order_acquire);
                    if (s == TASK_STATE_READY || s == TASK_STATE_RUNNING) {
                        // Wake already consumed this item, no need to rollback
                        // Item is orphaned in queue but marked as claimed below
                        // PopFromHead will skip it when it reaches this item
                        Policy::GetClaimed(item).store(true, std::memory_order_release);
                        return;
                    }
                }

                // Wake cleared is_waiting but hasn't set state yet
                // Mark item as claimed so it will be skipped
                Policy::GetClaimed(item).store(true, std::memory_order_release);

                // Roll back head to old_head (best effort)
                // Note: Another thread might have advanced head already
                T* expected = item;
                head_.compare_exchange_strong(expected, old_head,
                    std::memory_order_release, std::memory_order_relaxed);

                return;
            }

            // Successfully inserted at head
            // If this was the first item, also update tail
            if (!old_head) {
                T* expected = nullptr;
                tail_.compare_exchange_strong(expected, item,
                    std::memory_order_release, std::memory_order_relaxed);
            }
            return;
        }
        // CAS failed, retry
    }
}

template<typename T, typename Policy>
T* MpmcQueue<T, Policy>::PopFromHead() {
    // Spin threshold for empty queue waiting
    constexpr int MAX_EMPTY_SPINS = 1000;
    int empty_spin_count = 0;

    while (true) {
        T* head = head_.load(std::memory_order_acquire);

        // Empty queue check
        if (!head) {
            T* tail = tail_.load(std::memory_order_acquire);
            if (!tail) {
                return nullptr;  // Truly empty - OK to return nullptr
            }

            // tail != null but head == null: item being enqueued
            // Wait for head to be set, DO NOT return nullptr
            if (++empty_spin_count < MAX_EMPTY_SPINS) {
                BTHREAD_MPMC_PAUSE();
            } else {
                std::this_thread::yield();
                empty_spin_count = 0;  // Reset counter after yield
            }
            continue;  // Keep retrying, never return nullptr here
        }

        // Has items, reset empty counter
        empty_spin_count = 0;

        // Try to claim this item first (ABA prevention)
        if (Policy::GetClaimed(head).exchange(true, std::memory_order_acq_rel)) {
            // Already claimed by another consumer, try to skip
            T* next = Policy::GetNext(head).load(std::memory_order_acquire);
            if (next) {
                // Try to advance head past this claimed item
                T* expected = head;
                head_.compare_exchange_weak(expected, next,
                    std::memory_order_acq_rel, std::memory_order_relaxed);
            } else {
                // No next, check if queue is truly empty
                T* tail = tail_.load(std::memory_order_acquire);
                if (tail == head) {
                    // Queue is empty (head and tail both point to claimed item)
                    T* expected_head = head;
                    if (head_.compare_exchange_strong(expected_head, nullptr,
                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        // Use CAS for tail update to avoid race with AddToTail
                        // which may have set tail to a new item after our head CAS
                        T* expected_tail = head;
                        tail_.compare_exchange_strong(expected_tail, nullptr,
                            std::memory_order_acq_rel, std::memory_order_relaxed);
                    }
                }
                // Brief pause before retry
                BTHREAD_MPMC_PAUSE();
            }
            continue;  // Retry, don't return nullptr
        }

        // Successfully claimed, load next
        T* next = Policy::GetNext(head).load(std::memory_order_acquire);

        // If next is null, this is the last item - need to update tail too
        if (!next) {
            // Try to advance head and clear tail atomically
            T* expected_head = head;
            if (head_.compare_exchange_strong(expected_head, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Head advanced successfully, now clear tail
                // Use CAS to ensure we only clear tail if it still points to our item
                T* expected_tail = head;
                tail_.compare_exchange_strong(expected_tail, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed);

                return head;
            }
            // CAS failed (rare), release claim and retry
            Policy::GetClaimed(head).store(false, std::memory_order_relaxed);
            BTHREAD_MPMC_PAUSE();
            continue;
        }

        // Try to advance head (there's a next item)
        T* expected = head;
        if (head_.compare_exchange_strong(expected, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Successfully claimed and advanced
            return head;
        }

        // CAS failed (rare), release claim and retry
        Policy::GetClaimed(head).store(false, std::memory_order_relaxed);
        BTHREAD_MPMC_PAUSE();
        // Retry, don't return nullptr
    }
}

template<typename T, typename Policy>
int MpmcQueue<T, Policy>::PopMultipleFromHead(T** buffer, int max_count) {
    int count = 0;
    while (count < max_count) {
        T* item = PopFromHead();
        if (!item) break;
        buffer[count++] = item;
    }
    return count;
}

template<typename T, typename Policy>
void MpmcQueue<T, Policy>::Remove(T* item, std::atomic<bool>* is_waiting) {
    if (!is_waiting) return;

    // First mark as not waiting atomically
    bool expected = true;
    if (!is_waiting->compare_exchange_strong(expected, false,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;  // Already removed or not in queue
    }

    // Mark item as claimed so PopFromHead will skip it
    Policy::GetClaimed(item).store(true, std::memory_order_release);

    // Note: We don't actually remove from linked structure
    // PopFromHead will handle it when it reaches this item
}

// Explicit instantiation for ButexWaiterQueue (TaskMeta with MpmcEmbeddedNodePolicy)
template class MpmcQueue<TaskMeta, MpmcEmbeddedNodePolicy<TaskMeta>>;

} // namespace bthread