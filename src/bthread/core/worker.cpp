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

// Enable tracing for debugging
#define FUTEX_TRACE 0
#ifdef FUTEX_TRACE
#include <chrono>

namespace bthread {

struct TraceEntry {
    int thread_id;
    int event_type;
    int wake_count;
    int wake_pending;
    int is_idle;
    uint64_t timestamp;
};
static constexpr int MAX_TRACE = 10000;
static std::atomic<int> trace_idx{0};
static TraceEntry trace_buffer[MAX_TRACE];

static void Trace(int thread_id, int event_type, int wake_count, int wake_pending, int is_idle) {
    int idx = trace_idx.fetch_add(1);
    if (idx < MAX_TRACE) {
        trace_buffer[idx] = {
            thread_id, event_type, wake_count, wake_pending, is_idle,
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        };
    }
}

void PrintFutexTrace() {
    fprintf(stderr, "\n=== Futex Trace Log ===\n");
    for (int i = 0; i < trace_idx.load() && i < MAX_TRACE; ++i) {
        auto& e = trace_buffer[i];
        const char* event_names[] = {"", "WAIT_START", "WAIT_CHECK", "FUTEX_WAIT", "FUTEX_RET", "WAKE_START", "WAKE_END"};
        fprintf(stderr, "[%05ld] T%d %s: wc=%d, wp=%d, idle=%d\n",
                static_cast<long>(e.timestamp % 100000000), e.thread_id, event_names[e.event_type],
                e.wake_count, e.wake_pending, e.is_idle);
    }
}

} // namespace bthread

#else
namespace bthread {
static void Trace(int, int, int, int, int) {}
void PrintFutexTrace() {}
} // namespace bthread
#endif

namespace bthread {

using namespace platform;

thread_local Worker* Worker::current_worker_ = nullptr;

Worker::Worker(int id) : id_(id) {
    std::memset(&saved_context_, 0, sizeof(saved_context_));
    std::memset(stack_pool_, 0, sizeof(stack_pool_));
    std::memset(task_cache_, 0, sizeof(task_cache_));
}

Worker::~Worker() {
    // Drain stack pool and task cache before destruction
    DrainStackPool();
    DrainTaskCache();
}

Worker* Worker::Current() {
    return current_worker_;
}


void Worker::Run() {
    current_worker_ = this;

    // Signal that this worker is ready
    Scheduler::Instance().WorkerReady();

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
        Trace(id_, 1, wake_count_.load(), wake_pending_.load(), is_idle_.load());

        is_idle_.store(true, std::memory_order_seq_cst);

        // Register this worker as idle before futex wait
        Scheduler::Instance().RegisterIdleWorker(id_);

        // Capture current generation with seq_cst
        int expected = wake_count_.load(std::memory_order_seq_cst);

        Trace(id_, 2, expected, wake_pending_.load(), 1);

        // Re-check queue after setting is_idle_ - catches concurrent task submission
        // Also check wake_pending_ - catches WakeUp that didn't see our is_idle_
        if (!local_queue_.Empty() ||
            !Scheduler::Instance().global_queue().Empty() ||
            wake_pending_.load(std::memory_order_seq_cst)) {
            is_idle_.store(false, std::memory_order_release);
            spin_count = 0;
            continue;
        }

        Trace(id_, 3, expected, wake_pending_.load(), 1);

        // Now wait for either:
        // - wake_count to change (WakeUp incremented it)
        // - FutexWake signal from WakeUp
        int result = platform::FutexWait(&wake_count_, expected, &ts);

        Trace(id_, 4, wake_count_.load(), wake_pending_.load(), 0);

        // Clear idle flag after waking
        is_idle_.store(false, std::memory_order_release);

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
    Trace(id_, 5, wake_count_.load(), wake_pending_.load(), is_idle_.load());

    // Set wake_pending_ first to prevent missed-wake race
    // If waiter checks is_idle_ before we set wake_pending_, they will see
    // wake_pending_ = true and skip FutexWait
    wake_pending_.store(true, std::memory_order_seq_cst);

    // Increment wake_count (the generation counter)
    wake_count_.fetch_add(1, std::memory_order_seq_cst);

    // ALWAYS call FutexWake, even if is_idle_ is false.
    // This handles the race where:
    // 1. Worker is about to go idle (has set is_idle_=true)
    // 2. WakeUp checks is_idle_ and sees false (race)
    // 3. Worker enters FutexWait
    // 4. WakeUp doesn't call FutexWake -> missed wake!
    //
    // The cost of an extra FutexWake is minimal (it's a no-op if no waiters).
    platform::FutexWake(&wake_count_, 1);

    Trace(id_, 6, wake_count_.load(), 1, is_idle_.load());

    // Clear wake_pending_ after potentially waking
    wake_pending_.store(false, std::memory_order_release);
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

TaskMeta* Worker::AcquireTaskMeta() {
    // 1. Try local cache
    if (task_cache_count_ > 0) {
        return task_cache_[--task_cache_count_];
    }

    // 2. Cache empty - refill from TaskGroup
    RefillTaskCache();
    if (task_cache_count_ > 0) {
        return task_cache_[--task_cache_count_];
    }

    // 3. TaskGroup exhausted - return nullptr
    return nullptr;
}

void Worker::ReleaseTaskMeta(TaskMeta* meta) {
    if (!meta) return;

    // 1. Try return to local cache
    if (task_cache_count_ < TASK_CACHE_SIZE) {
        task_cache_[task_cache_count_++] = meta;
        return;
    }

    // 2. Cache full - return to TaskGroup
    GetTaskGroup().DeallocTaskMeta(meta);
}

void Worker::RefillTaskCache() {
    int32_t slots[TASK_CACHE_SIZE];
    int count = GetTaskGroup().AllocMultipleSlots(slots, TASK_CACHE_SIZE);

    for (int i = 0; i < count; ++i) {
        TaskMeta* meta = GetTaskGroup().GetOrCreateTaskMeta(slots[i]);
        if (meta) {
            task_cache_[task_cache_count_++] = meta;
        }
    }
}

void Worker::DrainTaskCache() {
    for (int i = 0; i < task_cache_count_; ++i) {
        if (task_cache_[i]) {
            GetTaskGroup().DeallocTaskMeta(task_cache_[i]);
            task_cache_[i] = nullptr;
        }
    }
    task_cache_count_ = 0;
}

void Worker::Stop() {
    stop_flag_.store(1, std::memory_order_seq_cst);
    // Clear idle flag to ensure WakeUp will work
    is_idle_.store(false, std::memory_order_release);
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

    // Note: Keep stack with TaskMeta for reuse
    // The stack pool is used when TaskMeta doesn't have a suitable stack

    // Release reference - return to cache if ref count reaches 0
    if (task->Release()) {
        ReleaseTaskMeta(task);
    }
}

} // namespace bthread