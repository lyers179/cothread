#include "bthread/worker.h"
#include "bthread/scheduler.h"
#include "bthread/task_meta.h"
#include "bthread/core/task_meta_base.hpp"
#include "bthread/task_group.h"
#include "bthread/butex.h"
#include "bthread/platform/platform.h"

#include <random>
#include <cstring>

namespace bthread {

using namespace platform;

thread_local Worker* Worker::current_worker_ = nullptr;

Worker::Worker(int id) : id_(id) {
    std::memset(&saved_context_, 0, sizeof(saved_context_));
}

Worker::~Worker() {
    // Thread cleanup handled by scheduler
}

Worker* Worker::Current() {
    return current_worker_;
}

void Worker::Run() {
    current_worker_ = this;

    while (Scheduler::Instance().running()) {
        TaskMeta* task = PickTask();
        if (task == nullptr) {
            WaitForTask();
            continue;
        }

        current_task_ = task;
        task->state.store(TaskState::RUNNING, std::memory_order_release);
        task->local_worker = this;

        // Switch to bthread
        platform::SwapContext(&saved_context_, &task->context);

        // Returned from bthread
        TaskMeta* completed_task = current_task_;
        current_task_ = nullptr;

        // Handle task based on its new state
        HandleTaskAfterRun(completed_task);
    }
}

TaskMeta* Worker::PickTask() {
    TaskMeta* task;

    // 1. Local queue
    task = local_queue_.Pop();
    if (task) return task;

    // 2. Global queue (returns TaskMetaBase*, cast to TaskMeta* for bthread)
    TaskMetaBase* base_task = Scheduler::Instance().global_queue().Pop();
    if (base_task) {
        // In current implementation, global queue only has TaskMeta (bthread)
        // CoroutineMeta is handled by CoroutineScheduler
        if (base_task->type == TaskType::BTHREAD) {
            task = static_cast<TaskMeta*>(base_task);
        } else {
            // CoroutineMeta - should not happen in current bthread-only worker
            // For unified scheduler (Phase 2), this will be handled properly
            task = nullptr;
        }
        if (task) return task;
    }

    // 3. Random work stealing
    int32_t wc = Scheduler::Instance().worker_count();
    if (wc <= 1) return nullptr;

    int attempts = wc * 3;
    static thread_local std::mt19937 rng(std::random_device{}());

    for (int i = 0; i < attempts; ++i) {
        int victim = (id_ + rng()) % wc;
        if (victim == id_) continue;

        Worker* other = Scheduler::Instance().GetWorker(victim);
        if (other) {
            task = other->local_queue().Steal();
            if (task) return task;
        }
    }

    return nullptr;
}

void Worker::SuspendCurrent() {
    platform::SwapContext(&current_task_->context, &saved_context_);
}

void Worker::Resume(TaskMeta* task) {
    (void)task;
    // Resume is handled by the scheduler loop
    // Task is already in a queue and will be picked up
}

void Worker::WaitForTask() {
    sleeping_.store(true, std::memory_order_release);

    // Double-check for tasks
    if (!local_queue_.Empty() ||
        !Scheduler::Instance().global_queue().Empty()) {
        sleeping_.store(false, std::memory_order_relaxed);
        return;
    }

    // Check if we should exit
    if (!Scheduler::Instance().running()) {
        sleeping_.store(false, std::memory_order_relaxed);
        return;
    }

    // Read sleep_token AFTER checking queues
    // This ensures we don't miss a wake-up that happened after the check
    int expected = sleep_token_.load(std::memory_order_acquire);

    // Triple-check for tasks AFTER reading sleep_token
    // This prevents the race where a wake-up happens between queue check and FutexWait
    if (!local_queue_.Empty() ||
        !Scheduler::Instance().global_queue().Empty()) {
        sleeping_.store(false, std::memory_order_relaxed);
        return;
    }

    // Sleep using platform futex with timeout to periodically check running state
    platform::timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 100000000;  // 100ms timeout
    platform::FutexWait(&sleep_token_, expected, &ts);

    sleeping_.store(false, std::memory_order_relaxed);
}

void Worker::WakeUp() {
    // Always increment sleep token to wake up any pending FutexWait
    // This is safe even if worker is not sleeping - it will just return from FutexWait
    sleep_token_.fetch_add(1, std::memory_order_release);
    platform::FutexWake(&sleep_token_, 1);
}

int Worker::YieldCurrent() {
    if (!current_task_) return EINVAL;

    current_task_->state.store(TaskState::READY, std::memory_order_release);
    local_queue_.Push(current_task_);
    SuspendCurrent();
    return 0;
}

void Worker::HandleTaskAfterRun(TaskMeta* task) {
    TaskState state = task->state.load(std::memory_order_acquire);

    switch (state) {
        case TaskState::FINISHED:
            HandleFinishedTask(task);
            break;

        case TaskState::SUSPENDED:
            // Task is waiting on butex, nothing to do
            break;

        case TaskState::READY:
            // Task yielded, already in queue
            break;

        default:
            // Should not happen
            break;
    }
}

void Worker::HandleFinishedTask(TaskMeta* task) {
    // Wake up any joiners
    if (task->join_waiters.load(std::memory_order_acquire) > 0 && task->join_butex) {
        // Increment generation before waking, so joiners can detect the change
        Butex* butex = static_cast<Butex*>(task->join_butex);
        butex->set_value(butex->value() + 1);
        Scheduler::Instance().WakeButex(task->join_butex, INT_MAX);
    }

    // Release reference
    if (task->Release()) {
        GetTaskGroup().DeallocTaskMeta(task);
    }
}

} // namespace bthread