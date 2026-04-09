#include "bthread/core/task_group.hpp"
#include "bthread/sync/butex.hpp"

#include <cstdlib>

namespace bthread {

TaskGroup::TaskGroup() {
    // Initialize free list - all slots are initially free
    for (int32_t i = 0; i < static_cast<int32_t>(POOL_SIZE); ++i) {
        free_slots_[i].store(i + 1, std::memory_order_relaxed);
        generations_[i].store(1, std::memory_order_relaxed);
    }
    // Last slot points to -1 (end of list)
    free_slots_[POOL_SIZE - 1].store(-1, std::memory_order_relaxed);
    free_head_.store(0, std::memory_order_relaxed);
}

TaskGroup::~TaskGroup() {
    // Deallocate all TaskMetas
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        TaskMeta* meta = task_pool_[i].load(std::memory_order_acquire);
        if (meta) {
            if (meta->stack) {
                platform::DeallocateStack(meta->stack, meta->stack_size);
            }
            delete meta;
        }
    }
}

TaskMeta* TaskGroup::AllocTaskMeta() {
    // Try free list first
    int32_t slot = free_head_.load(std::memory_order_acquire);
    while (slot >= 0) {
        int32_t next = free_slots_[slot].load(std::memory_order_relaxed);
        // Use relaxed on failure - we just retry anyway
        if (free_head_.compare_exchange_weak(slot, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Got a slot from free list
            TaskMeta* meta = task_pool_[slot].load(std::memory_order_relaxed);

            if (!meta) {
                // Allocate new TaskMeta
                meta = new TaskMeta();
                meta->slot_index = slot;
                meta->generation = generations_[slot].load(std::memory_order_relaxed);
                task_pool_[slot].store(meta, std::memory_order_release);
            } else {
                // Reuse existing TaskMeta
                meta->slot_index = slot;
                meta->generation = generations_[slot].load(std::memory_order_relaxed);
            }

            return meta;
        }
    }

    // Free list exhausted
    return nullptr;
}

int TaskGroup::AllocMultipleSlots(int32_t* slots, int count) {
    if (!slots || count <= 0) return 0;

    int allocated = 0;

    // Use CAS loop to atomically grab multiple slots
    while (allocated < count) {
        int32_t head = free_head_.load(std::memory_order_acquire);
        if (head < 0) {
            // Free list exhausted
            break;
        }

        // Walk free list to collect slots
        int32_t slots_to_claim[16];
        int32_t current = head;
        int found = 0;

        while (current >= 0 && found < count && found < 16) {
            slots_to_claim[found++] = current;
            current = free_slots_[current].load(std::memory_order_relaxed);
        }

        if (found == 0) break;

        // Try to claim by updating free_head
        int32_t new_head = current;
        if (free_head_.compare_exchange_weak(head, new_head,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Successfully claimed
            for (int i = 0; i < found && allocated < count; ++i) {
                slots[allocated++] = slots_to_claim[i];
            }
            break;
        }
        // CAS failed, retry
    }

    return allocated;
}

TaskMeta* TaskGroup::GetOrCreateTaskMeta(int32_t slot) {
    if (slot < 0 || slot >= static_cast<int32_t>(POOL_SIZE)) {
        return nullptr;
    }

    TaskMeta* meta = task_pool_[slot].load(std::memory_order_acquire);
    if (meta) {
        // Update slot_index and generation
        meta->slot_index = slot;
        meta->generation = generations_[slot].load(std::memory_order_relaxed);
        return meta;
    }

    // Need to create new TaskMeta
    meta = new TaskMeta();
    meta->slot_index = slot;
    meta->generation = generations_[slot].load(std::memory_order_relaxed);
    task_pool_[slot].store(meta, std::memory_order_release);
    return meta;
}

void TaskGroup::DeallocTaskMeta(TaskMeta* task) {
    if (!task) return;

    uint32_t slot = task->slot_index;

    // Clean up join_butex if allocated
    if (task->join_butex) {
        delete static_cast<bthread::Butex*>(task->join_butex);
        task->join_butex = nullptr;
    }

    // Increment generation for next use
    uint32_t new_gen = generations_[slot].fetch_add(1, std::memory_order_relaxed) + 1;
    task->generation = new_gen;

    // Reset state (keep stack for reuse)
    task->state.store(TaskState::READY, std::memory_order_relaxed);
    task->ref_count.store(0, std::memory_order_relaxed);
    task->fn = nullptr;
    task->arg = nullptr;
    task->result = nullptr;
    task->join_waiters.store(0, std::memory_order_relaxed);
    task->waiting_butex = nullptr;
    task->waiter.next.store(nullptr, std::memory_order_relaxed);
    task->waiter.wakeup.store(false, std::memory_order_relaxed);
    task->waiter.timed_out.store(false, std::memory_order_relaxed);
    task->waiter.deadline_us = 0;
    task->waiter.timer_id = 0;
    task->local_worker = nullptr;
    task->next = nullptr;

    // Add to free list
    int32_t old_head = free_head_.load(std::memory_order_relaxed);
    do {
        free_slots_[slot].store(old_head, std::memory_order_relaxed);
    } while (!free_head_.compare_exchange_weak(old_head, slot,
            std::memory_order_release, std::memory_order_relaxed));
}

TaskMeta* TaskGroup::DecodeId(bthread_t tid) const {
    uint32_t slot = static_cast<uint32_t>(tid & 0xFFFFFFFF);
    uint32_t gen = static_cast<uint32_t>(tid >> 32);

    if (slot >= POOL_SIZE) return nullptr;

    TaskMeta* meta = task_pool_[slot].load(std::memory_order_acquire);
    if (meta && meta->generation == gen) {
        return meta;
    }
    return nullptr;  // Stale bthread_t
}

TaskGroup& GetTaskGroup() {
    static TaskGroup instance;
    return instance;
}

std::vector<TaskMeta*> TaskGroup::GetSuspendedTasks() const {
    std::vector<TaskMeta*> suspended;
    // Iterate through all slots and find SUSPENDED tasks
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        TaskMeta* meta = task_pool_[i].load(std::memory_order_acquire);
        if (meta) {
            TaskState state = meta->state.load(std::memory_order_acquire);
            if (state == TaskState::SUSPENDED) {
                suspended.push_back(meta);
            }
        }
    }
    return suspended;
}

} // namespace bthread