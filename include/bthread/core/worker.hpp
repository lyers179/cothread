// include/bthread/core/worker.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "bthread/core/task_meta_base.hpp"
#include "bthread/queue/work_stealing_queue.hpp"
#include "bthread/platform/platform.h"

// Forward declarations
namespace bthread {
class Scheduler;
struct TaskMeta;
}

namespace coro {
struct CoroutineMeta;
}

namespace bthread {

/**
 * @brief Worker represents a pthread worker thread that executes both bthread and coroutine tasks.
 *
 * Each worker has:
 * - A local work-stealing queue for bthread tasks
 * - Access to the global queue for both task types
 * - A saved context for switching back from bthread tasks
 */
class Worker {
public:
    explicit Worker(int id);
    ~Worker();

    // Disable copy and move
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Main worker loop - handles both bthread and coroutine tasks
    void Run();

    // Pick next task to run (returns TaskMetaBase* for unified handling)
    TaskMetaBase* PickTask();

    // Suspend current task and return to scheduler
    void SuspendCurrent();

    // Resume a task
    void Resume(TaskMetaBase* task);

    // Wait for task when idle
    void WaitForTask();

    // Wake up sleeping worker
    void WakeUp();

    // Stop worker (set stop flag and wake up)
    void Stop();

    // Check if worker is stopped
    bool IsStopped() const {
        return stop_flag_.load(std::memory_order_seq_cst) != 0;
    }

    // Yield current task
    int YieldCurrent();

    // Batch management
    void MaybeFlushBatch();

    // Accessors
    int id() const { return id_; }
    TaskMetaBase* current_task() const { return current_task_; }
    WorkStealingQueue& local_queue() { return local_queue_; }
    const WorkStealingQueue& local_queue() const { return local_queue_; }
    platform::ThreadId thread() const { return thread_; }
    void set_thread(platform::ThreadId tid) { thread_ = tid; }

    // Stack pool configuration (public for testing)
    static constexpr int STACK_POOL_SIZE = 8;
    static constexpr size_t DEFAULT_STACK_SIZE = 8192;

    // TaskMeta cache configuration
    static constexpr int TASK_CACHE_SIZE = 4;

    // Stack pool operations
    void* AcquireStack(size_t size = DEFAULT_STACK_SIZE);
    void ReleaseStack(void* stack_top, size_t size);
    int stack_pool_count() const { return stack_pool_count_; }
    void DrainStackPool();

    // TaskMeta cache operations
    TaskMeta* AcquireTaskMeta();
    void ReleaseTaskMeta(TaskMeta* meta);
    int task_cache_count() const { return task_cache_count_; }
    void DrainTaskCache();
    void RefillTaskCache();

    // Get current worker (thread-local)
    static Worker* Current();

private:
    static constexpr int BATCH_SIZE = 8;

    // Handle task after it finishes running
    void HandleTaskAfterRun(TaskMetaBase* task);

    // Handle finished bthread task
    void HandleFinishedBthread(TaskMeta* task);

    // Run a bthread task
    void RunBthread(TaskMeta* task);

    // Run a coroutine task
    void RunCoroutine(coro::CoroutineMeta* meta);

    int id_;
    platform::ThreadId thread_;
    WorkStealingQueue local_queue_;
    TaskMetaBase* current_task_{nullptr};
    platform::Context saved_context_{};

    // Batch for reducing queue operations
    TaskMetaBase* local_batch_[BATCH_SIZE];
    int batch_count_{0};

    // Stack pool - reusable stacks to avoid mmap/munmap overhead
    void* stack_pool_[STACK_POOL_SIZE];
    int stack_pool_count_{0};

    // TaskMeta cache - reusable TaskMetas to avoid global CAS contention
    TaskMeta* task_cache_[TASK_CACHE_SIZE];
    int task_cache_count_{0};

    // Stop flag - set to non-zero to stop the worker
    std::atomic<int> stop_flag_{0};

    // Wake counter - increments on each WakeUp, also increments on Stop
    // WaitOnAddress on this counter handles races inherently
    std::atomic<int> wake_count_{0};

    // Idle flag - true when worker is waiting in futex
    std::atomic<bool> is_idle_{false};

    static thread_local Worker* current_worker_;
};

} // namespace bthread