#pragma once

#include <atomic>
#include <cstdint>

#include "bthread/task_meta.h"

namespace bthread {

// Lock-free double-ended queue with ABA prevention
class WorkStealingQueue {
public:
    static constexpr size_t CAPACITY = 1024;

    WorkStealingQueue();
    ~WorkStealingQueue() = default;

    // Disable copy and move
    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    // Push task to tail (owner only)
    void Push(TaskMeta* task);

    // Pop task from tail (owner only)
    TaskMeta* Pop();

    // Steal task from head (thief only)
    TaskMeta* Steal();

    // Check if empty (approximate)
    bool Empty() const;

private:
    // Helper for packing/unpacking version and index
    static uint32_t ExtractIndex(uint64_t v) { return v & 0xFFFFFFFF; }
    static uint32_t ExtractVersion(uint64_t v) { return v >> 32; }
    static uint64_t MakeVal(uint32_t ver, uint32_t idx) {
        return (static_cast<uint64_t>(ver) << 32) | idx;
    }

    std::atomic<TaskMeta*> buffer_[CAPACITY];
    std::atomic<uint64_t> head_{0};  // [version:32 | index:32]
    std::atomic<uint64_t> tail_{0};  // [version:32 | index:32]
};

} // namespace bthread