// src/worker.cpp
#include "bthread/worker.h"
#include "bthread/scheduler.h"
#include "bthread/task_meta.h"
#include "bthread/core/task_meta_base.hpp"
#include "bthread/task_group.h"
#include "bthread/butex.h"
#include "bthread/platform/platform.h"
#include "coro/meta.h"

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

    int task_count = 0;
    while (!IsStopped()) {
        TaskMetaBase* task = PickTask();
        if (task == nullptr) {
            if (IsStopped()) {
                break;
            }
            WaitForTask();
            if (IsStopped()) {
                break;
            }
            continue;
        }

        current_task_ = task;
        task->state.store(TaskState::RUNNING, std::memory_order_release);
        task->owner_worker = this;

        switch (task->type) {
            case TaskType::BTHREAD:
                RunBthread(static_cast<TaskMeta*>(task));
                break;
            case TaskType::COROUTINE:
                RunCoroutine(static_cast<coro::CoroutineMeta*>(task));
                break;
        }

        TaskMetaBase* completed_task = current_task_;
        current_task_ = nullptr;

        task_count++;

        // Periodically check for stop even when processing many tasks
        if (task_count >= 10) {
            task_count = 0;
            if (IsStopped()) {
                HandleTaskAfterRun(completed_task);
                break;
            }
        }

        HandleTaskAfterRun(completed_task);
    }

    // Worker is exiting
    // Ensure no current task
    current_task_ = nullptr;
}

void Worker::RunBthread(TaskMeta* task) {
    platform::SwapContext(&saved_context_, &task->context);
}

void Worker::RunCoroutine(coro::CoroutineMeta* meta) {
    // Resume the coroutine
    if (meta->handle && !meta->handle.done()) {
        meta->handle.resume();
    }
}

int Worker::YieldCurrent() {
    if (!current_task_) return EINVAL;

    current_task_->state.store(TaskState::READY, std::memory_order_release);

    // Add to batch instead of queue
    local_batch_[batch_count_++] = current_task_;

    // Flush if batch is full
    MaybeFlushBatch();

    SuspendCurrent();
    return 0;
}

void Worker::MaybeFlushBatch() {
    if (batch_count_ >= BATCH_SIZE) {
        for (int i = 0; i < batch_count_; ++i) {
            local_queue_.Push(local_batch_[i]);
        }
        batch_count_ = 0;
    }
}

TaskMetaBase* Worker::PickTask() {
    // 1. Try batch first (LIFO for cache locality)
    if (batch_count_ > 0) {
        return local_batch_[--batch_count_];
    }

    // 2. Try local queue with batch prefill
    TaskMetaBase* task = local_queue_.Pop();
    if (task) {
        local_batch_[batch_count_++] = task;
        // Prefill batch
        for (int i = 0; i < BATCH_SIZE - 1 && batch_count_ < BATCH_SIZE; ++i) {
            TaskMetaBase* t2 = local_queue_.Pop();
            if (t2) {
                local_batch_[batch_count_++] = t2;
            } else {
                break;
            }
        }
        return local_batch_[--batch_count_];
    }

    // 3. Try global queue
    task = Scheduler::Instance().global_queue().Pop();
    if (task) return task;

    // 4. Try work stealing
    int32_t wc = Scheduler::Instance().worker_count();
    if (wc <= 1) return nullptr;

    int attempts = wc * 3;
    static thread_local std::mt19937 rng(std::random_device{}());

    for (int i = 0; i < attempts; ++i) {
        int victim = (id_ + rng()) % wc;
        if (victim == id_) continue;

        Worker* other = Scheduler::Instance().GetWorker(victim);
        if (other) {
            task = other->local_queue_.Steal();
            if (task) return task;
        }
    }

    return nullptr;
}

void Worker::SuspendCurrent() {
    // For bthread, swap back to worker context
    if (current_task_->type == TaskType::BTHREAD) {
        platform::SwapContext(&static_cast<TaskMeta*>(current_task_)->context, &saved_context_);
    }
    // For coroutine, the handle naturally returns to the caller
}

void Worker::Resume(TaskMetaBase* task) {
    (void)task;
    // Resume is handled by the scheduler loop
    // Task is already in a queue and will be picked up
}

void Worker::WaitForTask() {
    // Wait on wake_count_ with timeout to handle race conditions
    // Timeout ensures we periodically check the stop flag

    platform::timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;  // 1ms timeout for faster response

    while (stop_flag_.load(std::memory_order_seq_cst) == 0) {
        // Check for tasks first
        if (!local_queue_.Empty() ||
            !Scheduler::Instance().global_queue().Empty()) {
            return;
        }

        // Read current wake_count before waiting
        int expected = wake_count_.load(std::memory_order_acquire);

        // Wait for wake_count_ to change or timeout
        int result = platform::FutexWait(&wake_count_, expected, &ts);

        // On timeout, re-check stop flag and continue
        if (result == ETIMEDOUT) {
            continue;
        }
    }
}

void Worker::WakeUp() {
    wake_count_.fetch_add(1, std::memory_order_release);
    platform::FutexWake(&wake_count_, 1);
}

void Worker::Stop() {
    stop_flag_.store(1, std::memory_order_seq_cst);
    wake_count_.fetch_add(1, std::memory_order_seq_cst);
    // Wake ALL waiting threads on this wake_count_
    platform::FutexWake(&wake_count_, INT_MAX);
}

void Worker::HandleTaskAfterRun(TaskMetaBase* task) {
    TaskState state = task->state.load(std::memory_order_acquire);

    switch (state) {
        case TaskState::FINISHED:
            if (task->type == TaskType::BTHREAD) {
                HandleFinishedBthread(static_cast<TaskMeta*>(task));
            }
            // Coroutine cleanup is handled by the coroutine framework
            break;

        case TaskState::SUSPENDED:
            // Task is waiting on sync primitive, nothing to do
            break;

        case TaskState::READY:
            // Task yielded, already in queue
            break;

        default:
            // Should not happen
            break;
    }
}

void Worker::HandleFinishedBthread(TaskMeta* task) {
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