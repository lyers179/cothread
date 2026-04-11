#include "bthread/queue/execution_queue.hpp"

namespace bthread {

ExecutionQueue::ExecutionQueue() = default;

ExecutionQueue::~ExecutionQueue() {
    Stop();
}

void ExecutionQueue::Stop() {
    stopped_.store(true, std::memory_order_release);

    // Execute any remaining tasks
    Execute();
}

bool ExecutionQueue::ExecuteOne() {
    if (stopped_.load(std::memory_order_acquire)) {
        return false;
    }

    // Pop from MpscQueue (lock-free, single consumer)
    TaskWrapper* wrapper = queue_.Pop();
    if (!wrapper) {
        pending_.store(false, std::memory_order_release);
        return false;
    }

    Task task = std::move(wrapper->task);
    delete wrapper;

    if (task) {
        task();
        return true;
    }
    return false;
}

int ExecutionQueue::Execute() {
    int executed = 0;

    while (ExecuteOne()) {
        ++executed;
    }

    return executed;
}

void ExecutionQueue::Submit(Task task) {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }

    // Create wrapper and push to MpscQueue (lock-free, multi-producer)
    TaskWrapper* wrapper = new TaskWrapper();
    wrapper->task = std::move(task);

    queue_.Push(wrapper);
    pending_.store(true, std::memory_order_release);
}

} // namespace bthread