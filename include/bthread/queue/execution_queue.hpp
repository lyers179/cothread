#pragma once

#include <functional>
#include <atomic>

#include "bthread/queue/mpsc_queue.hpp"
#include "bthread/pool/object_pool.hpp"
#include "bthread/platform/platform.h"

namespace bthread {

// Execution queue for ordered task execution
// Now uses lock-free MPSC queue internally
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
        return pending_.load(std::memory_order_acquire);
    }

private:
    // Internal wrapper structure for MpscQueue (intrusive queue requires `next` member)
    struct TaskWrapper {
        Task task;
        std::atomic<TaskWrapper*> next{nullptr};      // MpscQueue linkage
        std::atomic<TaskWrapper*> pool_next{nullptr}; // ObjectPool linkage (mutually exclusive with queue)
    };

    MpscQueue<TaskWrapper> queue_;  // Lock-free MPSC queue
    ObjectPool<TaskWrapper> pool_{64};  // Object pool for TaskWrapper
    std::atomic<bool> stopped_{false};
    std::atomic<bool> pending_{false};
};

} // namespace bthread