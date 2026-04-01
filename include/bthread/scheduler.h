// include/bthread/scheduler.h
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <thread>

#include "bthread/core/task_meta_base.hpp"
#include "bthread/task_meta.h"
#include "bthread/task_group.h"
#include "bthread/worker.h"
#include "bthread/global_queue.h"

// Forward declarations for coroutine support
namespace coro {
template<typename T> class Task;
template<typename T> class SafeTask;
struct CoroutineMeta;
}

namespace bthread {

// Forward declarations
class TimerThread;
class Butex;

/**
 * @brief Unified scheduler managing both bthread and coroutine tasks.
 *
 * Implements M:N threading model where M user-space tasks (bthread or coroutine)
 * are multiplexed onto N OS threads (workers).
 *
 * Thread Safety:
 * - Init(): Thread-safe via std::call_once. Can be called from any thread.
 * - Shutdown(): Thread-safe. Can be called from any thread.
 * - Submit(): Thread-safe. Can be called from any thread.
 * - Spawn(): Thread-safe. Can be called from any thread concurrently.
 *
 * Usage:
 * - Call Init() before submitting tasks, or Submit() will auto-initialize.
 * - Call Shutdown() to stop worker threads (optional - destructor also calls it).
 */
class Scheduler {
public:
    /// Get singleton instance
    static Scheduler& Instance();

    /// Initialize scheduler with default worker count
    void Init();

    /// Initialize scheduler with specified worker count
    void Init(int worker_count);

    /// Shutdown scheduler
    void Shutdown();

    /// Check if running
    bool running() const {
        return running_.load(std::memory_order_acquire);
    }

    /// Check if initialized
    bool initialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

    /// Get global queue
    GlobalQueue& global_queue() { return global_queue_; }
    const GlobalQueue& global_queue() const { return global_queue_; }

    /// Get task group
    TaskGroup& task_group() { return GetTaskGroup(); }

    /// Get worker count
    int32_t worker_count() const {
        return worker_count_.load(std::memory_order_acquire);
    }

    /// Get worker by index
    Worker* GetWorker(int index);

    /// Set worker count (must be called before Init)
    void set_worker_count(int count) {
        configured_count_ = count;
    }

    /// Get timer thread (lazy init)
    TimerThread* GetTimerThread();

    /// Wake butex waiters
    void WakeButex(void* butex, int count);

    /// Wake idle workers
    void WakeIdleWorkers(int count);

    // ========== Unified Task Submission ==========

    /**
     * @brief Submit a task for execution (unified for bthread and coroutine).
     * Thread-safe: Can be called from any thread.
     * Auto-initializes the scheduler if not already initialized.
     * @param task The task to submit (TaskMeta or CoroutineMeta)
     */
    void Submit(TaskMetaBase* task);

    /**
     * @brief Enqueue a bthread task (legacy compatibility).
     * Thread-safe: Can be called from any thread.
     */
    void EnqueueTask(TaskMeta* task);

    // ========== Coroutine Support ==========
    // Template implementations are in scheduler.cpp

    /**
     * @brief Spawn a coroutine task for execution.
     * Thread-safe: Can be called from any thread concurrently.
     * Auto-initializes the scheduler if not already initialized.
     */
    template<typename T>
    coro::Task<T> Spawn(coro::Task<T> task);

    /**
     * @brief Spawn a SafeTask coroutine for execution.
     * Thread-safe: Can be called from any thread concurrently.
     */
    template<typename T>
    coro::SafeTask<T> Spawn(coro::SafeTask<T> task);

private:
    Scheduler();
    ~Scheduler();

    // Disable copy and move
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Start worker threads
    void StartWorkers(int count);

    // Wake all workers
    void WakeAllWorkers();

    std::vector<Worker*> workers_;
    std::mutex workers_mutex_;
    std::atomic<int32_t> worker_count_{0};
    int32_t configured_count_{0};

    GlobalQueue global_queue_;

    std::unique_ptr<TimerThread> timer_thread_;
    std::once_flag timer_init_flag_;
    std::mutex timer_mutex_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::once_flag init_once_;
};

} // namespace bthread