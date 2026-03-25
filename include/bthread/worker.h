#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "bthread/task_meta.h"
#include "bthread/work_stealing_queue.h"
#include "bthread/platform/platform.h"

namespace bthread {

// Forward declarations
class Scheduler;

// Worker represents a pthread worker thread
class Worker {
public:
    explicit Worker(int id);
    ~Worker();

    // Disable copy and move
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Main worker loop
    void Run();

    // Pick next task to run
    TaskMeta* PickTask();

    // Suspend current task and return to scheduler
    void SuspendCurrent();

    // Resume a task
    void Resume(TaskMeta* task);

    // Wait for task when idle
    void WaitForTask();

    // Wake up sleeping worker
    void WakeUp();

    // Yield current task
    int YieldCurrent();

    // Accessors
    int id() const { return id_; }
    TaskMeta* current_task() const { return current_task_; }
    WorkStealingQueue& local_queue() { return local_queue_; }
    const WorkStealingQueue& local_queue() const { return local_queue_; }
    platform::ThreadId thread() const { return thread_; }

    // Get current worker (thread-local)
    static Worker* Current();

private:
    // Handle task after it finishes running
    void HandleTaskAfterRun(TaskMeta* task);

    // Handle finished task
    void HandleFinishedTask(TaskMeta* task);

    int id_;
    platform::ThreadId thread_;
    WorkStealingQueue local_queue_;
    TaskMeta* current_task_{nullptr};
    platform::Context saved_context_{};

    // Sleep state
    std::atomic<bool> sleeping_{false};
    std::atomic<int> sleep_token_{0};

    static thread_local Worker* current_worker_;
};

} // namespace bthread