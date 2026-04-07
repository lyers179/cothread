// include/bthread/work_stealing_queue.h
#pragma once

#include <atomic>
#include <cstdint>

#include "bthread/core/task_meta_base.hpp"

namespace bthread {

/**
 * @brief Lock-free double-ended queue with ABA prevention for work stealing.
 *
 * Supports both bthread (TaskMeta) and coroutine (CoroutineMeta) tasks through
 * the unified TaskMetaBase interface.
 *
 * Thread Safety:
 * - Push(): Owner thread only
 * - Pop(): Owner thread only
 * - Steal(): Other threads (thieves)
 */
class WorkStealingQueue {
public:
    static constexpr size_t CAPACITY = 1024;
    static constexpr size_t CACHE_LINE_SIZE = 64;

    WorkStealingQueue();
    ~WorkStealingQueue() = default;

    // Disable copy and move
    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    /**
     * @brief Push task to tail (owner only).
     * @param task The task to push (TaskMeta or CoroutineMeta)
     */
    void Push(TaskMetaBase* task);

    /**
     * @brief Pop task from tail (owner only).
     * @return The popped task, or nullptr if empty
     */
    TaskMetaBase* Pop();

    /**
     * @brief Steal task from head (thief only).
     * @return The stolen task, or nullptr if empty
     */
    TaskMetaBase* Steal();

    /**
     * @brief Check if empty (approximate).
     */
    bool Empty() const;

private:
    // Helper for packing/unpacking version and index
    static uint32_t ExtractIndex(uint64_t v) { return v & 0xFFFFFFFF; }
    static uint32_t ExtractVersion(uint64_t v) { return v >> 32; }
    static uint64_t MakeVal(uint32_t ver, uint32_t idx) {
        return (static_cast<uint64_t>(ver) << 32) | idx;
    }

    std::atomic<TaskMetaBase*> buffer_[CAPACITY];

    // head on its own cache line
    alignas(CACHE_LINE_SIZE)
    std::atomic<uint64_t> head_{0};  // [version:32 | index:32]

    // tail on its own cache line
    alignas(CACHE_LINE_SIZE)
    std::atomic<uint64_t> tail_{0};  // [version:32 | index:32]
};

} // namespace bthread