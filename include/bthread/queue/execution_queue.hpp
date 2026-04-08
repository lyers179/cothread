#pragma once

#include <functional>
#include <mutex>
#include <atomic>
#include <queue>

#include "bthread/platform/platform.h"

namespace bthread {

// Execution queue for ordered task execution
// Tasks execute in the order they are submitted (single-threaded)
class ExecutionQueue {
public:
    using Task = std::function<void()>;

    ExecutionQueue();
    ~ExecutionQueue();

    // Disable copy and move
    ExecutionQueue(const ExecutionQueue&) = delete;
    ExecutionQueue& operator=(const ExecutionQueue&) = delete;

    // Execute all pending tasks and stop
    void Stop();

    // Execute one pending task (returns false if none)
    bool ExecuteOne();

    // Execute all pending tasks (returns number executed)
    int Execute();

    // Submit a task for execution
    void Submit(Task task);

    // Check if there are pending tasks
    bool HasPending() const {
        return !pending_.load(std::memory_order_acquire);
    }

private:
    std::queue<Task> tasks_;
    mutable std::mutex tasks_mutex_;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> pending_{false};
};

} // namespace bthread