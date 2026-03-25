#include "bthread/execution_queue.h"

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

    Task task;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        if (tasks_.empty()) {
            pending_.store(false, std::memory_order_release);
            return false;
        }
        task = std::move(tasks_.front());
        tasks_.pop();

        if (tasks_.empty()) {
            pending_.store(false, std::memory_order_release);
        }
    }

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

    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks_.push(std::move(task));
        pending_.store(true, std::memory_order_release);
    }
}

} // namespace bthread