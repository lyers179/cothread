#pragma once

#include <atomic>
#include <cstdint>

#include "bthread/task_meta.h"

namespace bthread {

// MPSC (Multi-Producer Single-Consumer) queue for global task distribution
class GlobalQueue {
public:
    GlobalQueue() = default;
    ~GlobalQueue() = default;

    // Disable copy and move
    GlobalQueue(const GlobalQueue&) = delete;
    GlobalQueue& operator=(const GlobalQueue&) = delete;

    // Push task to queue (multiple producers)
    void Push(TaskMeta* task);

    // Pop all tasks and return as linked list (single consumer)
    // Returns head of reversed list (first to execute)
    TaskMeta* Pop();

    // Check if empty
    bool Empty() const {
        return head_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<TaskMeta*> head_{nullptr};
    std::atomic<uint64_t> version_{0};  // For ABA prevention
};

} // namespace bthread