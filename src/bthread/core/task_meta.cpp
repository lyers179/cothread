// src/bthread/core/task_meta.cpp
#include "bthread/core/task_meta.hpp"
#include "bthread/core/worker.hpp"
#include "bthread/platform/platform.h"
#include "bthread.h"

namespace bthread {

void detail::BthreadEntry(void* arg) {
    TaskMeta* task = static_cast<TaskMeta*>(arg);
    task->result = task->fn(task->arg);
    bthread_exit(task->result);
}

void TaskMeta::resume() {
    // Resume execution of this bthread
    // Called by scheduler/worker when task should run

    // Note: The actual context switch is performed by Worker::Run()
    // This method is provided for unified TaskMetaBase interface
    // In the current bthread implementation, execution is managed by Worker

    // When unified scheduler is complete, this will handle:
    // 1. Setting state to RUNNING
    // 2. Performing context switch via Worker's saved_context

    // For now, tasks are enqueued and Worker::Run() handles the context switch
    if (state.load(std::memory_order_acquire) == TaskState::READY) {
        // Task is ready to run - it will be picked up by a worker
        // No direct action needed here in current implementation
    }
}

} // namespace bthread