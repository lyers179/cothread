#include "bthread/task_group.h"

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
        if (free_head_.compare_exchange_weak(slot, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
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

void TaskGroup::DeallocTaskMeta(TaskMeta* task) {
    if (!task) return;

    uint32_t slot = task->slot_index;

    // Increment generation for next use
    uint32_t new_gen = generations_[slot].fetch_add(1, std::memory_order_relaxed) + 1;
    task->generation = new_gen;

    // Reset state (keep stack for reuse)
    task->state.store(TaskState::READY, std::memory_order_relaxed);
    task->ref_count.store(0, std::memory_order_relaxed);
    task->fn = nullptr;
    task->arg = nullptr;
    task->result = nullptr;
    task->join_butex = nullptr;
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

} // namespace bthread