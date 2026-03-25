#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "bthread/task_meta.h"

namespace bthread {

// TaskGroup manages TaskMeta allocation and bthread_t mapping
class TaskGroup {
public:
    static constexpr size_t POOL_SIZE = 16384;

    TaskGroup();
    ~TaskGroup();

    // Disable copy and move
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    // Allocate TaskMeta from pool, returns nullptr if exhausted
    TaskMeta* AllocTaskMeta();

    // Recycle TaskMeta back to pool
    void DeallocTaskMeta(TaskMeta* task);

    // Encode bthread_t from slot index and generation
    static constexpr bthread_t EncodeId(uint32_t slot, uint32_t gen) {
        return (static_cast<uint64_t>(gen) << 32) | slot;
    }

    // Decode bthread_t to TaskMeta with generation check
    TaskMeta* DecodeId(bthread_t tid) const;

    // Get task by slot index (no generation check)
    TaskMeta* GetTaskBySlot(uint32_t slot) const {
        if (slot >= POOL_SIZE) return nullptr;
        return task_pool_[slot].load(std::memory_order_acquire);
    }

    int32_t worker_count() const {
        return worker_count_.load(std::memory_order_acquire);
    }

    void set_worker_count(int32_t count) {
        worker_count_.store(count, std::memory_order_release);
    }

private:
    // TaskMeta pool for reuse
    std::array<std::atomic<TaskMeta*>, POOL_SIZE> task_pool_;

    // Free list: stored as linked list using slot indices
    // free_slots_[i] points to next free slot, -1 terminates
    std::array<std::atomic<int32_t>, POOL_SIZE> free_slots_;
    std::atomic<int32_t> free_head_{-1};

    // Generation counters (per slot, for bthread_t encoding)
    std::array<std::atomic<uint32_t>, POOL_SIZE> generations_;

    std::atomic<int32_t> worker_count_{0};
};

// Singleton instance
TaskGroup& GetTaskGroup();

} // namespace bthread