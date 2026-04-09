// src/bthread/core/worker.cpp
#include "bthread/core/worker.hpp"
#include "bthread/core/scheduler.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/core/task_meta_base.hpp"
#include "bthread/core/task_group.hpp"
#include "bthread/sync/butex.hpp"
#include "bthread/platform/platform.h"
#include "coro/coroutine.h"

#include <cstring>
#include <functional>  // for std::hash
#include <thread>      // for std::this_thread

namespace bthread {

using namespace platform;

thread_local Worker* Worker::current_worker_ = nullptr;

Worker::Worker(int id) : id_(id) {
    std::memset(&saved_context_, 0, sizeof(saved_context_));
    std::memset(stack_pool_, 0, sizeof(stack_pool_));
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

    // Drain remaining tasks after stop to ensure cleanup
    // This is important for tasks that were woken during shutdown
    for (int drain_count = 0; drain_count < 100; ++drain_count) {
        TaskMetaBase* task = PickTask();
        if (task == nullptr) {
            break;  // No more tasks to process
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

        HandleTaskAfterRun(current_task_);
        current_task_ = nullptr;
    }

    // Worker is exiting
    // Ensure no current task
    current_task_ = nullptr;
}

void Worker::RunBthread(TaskMeta* task) {
    platform::SwapContext(&saved_context_, &task->context);
}

void Worker::RunCoroutine(coro::CoroutineMeta* meta) {
    // Set current coroutine meta before resuming
    coro::current_coro_meta_ = meta;
    meta->state.store(TaskState::RUNNING, std::memory_order_release);

    // Resume the coroutine
    if (meta->handle && !meta->handle.done()) {
        meta->handle.resume();
    }

    // Clear current coroutine meta after resuming
    coro::current_coro_meta_ = nullptr;

    // Update state if coroutine completed
    if (meta->handle && meta->handle.done()) {
        meta->state.store(TaskState::FINISHED, std::memory_order_release);
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

    // 2. Try local queue - batch pop for efficiency
    int popped = local_queue_.PopMultiple(local_batch_, BATCH_SIZE);
    if (popped > 0) {
        batch_count_ = popped;
        return local_batch_[--batch_count_];
    }

    // 3. Try global queue
    TaskMetaBase* task = Scheduler::Instance().global_queue().Pop();
    if (task) return task;

    // 4. Try work stealing
    int32_t wc = Scheduler::Instance().worker_count();
    if (wc <= 1) return nullptr;

    // Use lightweight XOR shift RNG instead of mt19937
    // Faster and sufficient for work stealing randomization
    static thread_local uint32_t rng_state = static_cast<uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ 0x2654435761u);

    int attempts = wc * 3;
    for (int i = 0; i < attempts; ++i) {
        // XOR shift - fast random number generation
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;

        int victim = rng_state % wc;
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
    // Adaptive spin before futex wait to reduce kernel calls
    constexpr int MAX_SPINS = 50;        // Spin 50 times before futex wait
    constexpr int SPIN_CHECK_INTERVAL = 5;  // Check queue every 5 spins

    platform::timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;  // 1ms timeout for faster response

    int spin_count = 0;

    while (stop_flag_.load(std::memory_order_seq_cst) == 0) {
        // Check for tasks first
        if (!local_queue_.Empty() ||
            !Scheduler::Instance().global_queue().Empty()) {
            return;
        }

        // Adaptive spinning phase
        if (spin_count < MAX_SPINS) {
            #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
            #else
            std::atomic_signal_fence(std::memory_order_acquire);
            #endif

            ++spin_count;

            // Periodically check queue during spin
            if (spin_count % SPIN_CHECK_INTERVAL == 0) {
                if (!local_queue_.Empty() ||
                    !Scheduler::Instance().global_queue().Empty()) {
                    return;  // Task available, exit wait
                }
            }
            continue;
        }

        // Spin complete, enter futex wait
        int expected = wake_count_.load(std::memory_order_acquire);
        int result = platform::FutexWait(&wake_count_, expected, &ts);

        // On timeout, reset spin counter and continue
        if (result == ETIMEDOUT) {
            spin_count = 0;
            continue;
        }

        // After waking from futex, reset spin counter
        spin_count = 0;
    }
}

void Worker::WakeUp() {
    wake_count_.fetch_add(1, std::memory_order_release);
    platform::FutexWake(&wake_count_, 1);
}

void* Worker::AcquireStack(size_t size) {
    // 1. Try local pool first (only for default size)
    if (stack_pool_count_ > 0 && size <= DEFAULT_STACK_SIZE) {
        return stack_pool_[--stack_pool_count_];
    }

    // 2. Pool empty or wrong size - allocate new via platform API
    return platform::AllocateStack(size);
}

void Worker::ReleaseStack(void* stack_top, size_t size) {
    if (!stack_top) return;

    // 1. Try return to local pool (only default size)
    if (stack_pool_count_ < STACK_POOL_SIZE && size == DEFAULT_STACK_SIZE) {
        stack_pool_[stack_pool_count_++] = stack_top;
        return;
    }

    // 2. Pool full or wrong size - deallocate
    platform::DeallocateStack(stack_top, size);
}

void Worker::DrainStackPool() {
    for (int i = 0; i < stack_pool_count_; ++i) {
        if (stack_pool_[i]) {
            platform::DeallocateStack(stack_pool_[i], DEFAULT_STACK_SIZE);
            stack_pool_[i] = nullptr;
        }
    }
    stack_pool_count_ = 0;
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