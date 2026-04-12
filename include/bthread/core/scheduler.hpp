// include/bthread/core/scheduler.hpp
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <thread>
#include <functional>

#include "bthread/core/task_meta_base.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/core/task_group.hpp"
#include "bthread/core/worker.hpp"
#include "bthread/queue/global_queue.hpp"

// Forward declarations
namespace bthread {
class TimerThread;
class Butex;
}

// Forward declarations for coroutine support
namespace coro {
template<typename T> class Task;
template<typename T> class SafeTask;
struct CoroutineMeta;
}

namespace bthread {

/**
 * @brief Unified scheduler managing both bthread and coroutine tasks.
 *
 * Implements M:N threading model where M user-space tasks (bthread or coroutine)
 * are multiplexed onto N OS threads (workers).
 *
 * Thread Safety:
 * - Init(): Thread-safe via std::call_once
 * - Shutdown(): Thread-safe
 * - Submit(): Thread-safe, can be called from any thread
 * - Spawn(): Thread-safe for coroutine tasks
 *
 * Usage:
 * - Call Init() before submitting tasks, or Submit() will auto-initialize
 * - Call Shutdown() to stop worker threads (optional - destructor also calls it)
 * - Use Submit() for raw TaskMetaBase* submission
 * - Use coro::co_spawn() for coroutine tasks
 */
class Scheduler {
    friend class Worker;  // Allow Worker to call RegisterIdleWorker
public:
    /// Get singleton instance
    static Scheduler& Instance();

    /// Initialize scheduler with default worker count (hardware_concurrency)
    void Init();

    /// Initialize scheduler with specified worker count
    void Init(int worker_count);

    /// Shutdown scheduler and join all worker threads
    void Shutdown();

    /// Check if scheduler is running
    bool running() const {
        return running_.load(std::memory_order_acquire);
    }

    /// Check if scheduler is initialized
    bool initialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

    /// Get global queue
    GlobalQueue& global_queue() { return global_queue_; }
    const GlobalQueue& global_queue() const { return global_queue_; }

    /// Get task group
    TaskGroup& task_group();

    /// Get worker count
    int32_t worker_count() const {
        return worker_count_.load(std::memory_order_acquire);
    }

    /// Get worker by index
    Worker* GetWorker(int index);

    /// Get timer thread (lazy init)
    TimerThread* GetTimerThread();

    /// Set worker count (must be called before Init)
    void set_worker_count(int count) {
        configured_count_ = count;
    }

    /// Wake butex waiters
    void WakeButex(void* butex, int count);

    /// Wake idle workers
    void WakeIdleWorkers(int count);

    /// Pop one idle worker from registry (returns -1 if none)
    int PopIdleWorker();

    /// Called by worker when it's ready (has entered Run loop)
    void WorkerReady();

    // ========== Unified Task Submission ==========

    /**
     * @brief Submit a task for execution.
     * Thread-safe: Can be called from any thread.
     * Auto-initializes the scheduler if not already initialized.
     * @param task The task to submit (must be TaskMeta or CoroutineMeta)
     */
    void Submit(TaskMetaBase* task);

    /**
     * @brief Enqueue a bthread task (legacy compatibility).
     * Thread-safe: Can be called from any thread.
     */
    void EnqueueTask(TaskMeta* task);

    // ========== Coroutine Support ==========

    /**
     * @brief Spawn a coroutine task for execution.
     * Thread-safe: Can be called from any thread.
     * Auto-initializes the scheduler if not already initialized.
     * @tparam T Return type of the coroutine
     * @param task The coroutine task to spawn
     * @return The spawned task (can be co_awaited or detached)
     */
    template<typename T>
    coro::Task<T> Spawn(coro::Task<T> task);

    /**
     * @brief Spawn a SafeTask coroutine for execution.
     * Thread-safe: Can be called from any thread.
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

    static constexpr int MAX_WORKERS = 256;

    std::vector<Worker*> workers_;
    std::mutex workers_mutex_;
    // Atomic array for lock-free worker access in WakeIdleWorkers
    std::atomic<Worker*> workers_atomic_[MAX_WORKERS];
    std::atomic<int32_t> worker_count_{0};
    int32_t configured_count_{0};

    // Worker ready synchronization
    std::atomic<int32_t> workers_ready_{0};
    std::mutex workers_ready_mutex_;
    std::condition_variable workers_ready_cv_;

    GlobalQueue global_queue_;

    // ========== Idle Worker Registry (Optimization 1) ==========
    // Lock-free linked list of idle workers
    std::atomic<int> idle_head_{-1};            // Head of idle list (-1 = empty)
    std::atomic<int> idle_next_[MAX_WORKERS];  // Next pointers for each worker

    // Helper method for idle registry
    void RegisterIdleWorker(int worker_id);    // Called by Worker before futex wait

    std::unique_ptr<TimerThread> timer_thread_;
    std::once_flag timer_init_flag_;
    std::mutex timer_mutex_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::once_flag init_once_;
};

} // namespace bthread

// ========== Coroutine Spawn Functions ==========

namespace coro {

/**
 * @brief Spawn a coroutine task for execution by the unified scheduler.
 * Thread-safe: Can be called from any thread concurrently.
 * Auto-initializes the scheduler if not already initialized.
 * @tparam T Return type of the coroutine
 * @param task The coroutine task to spawn
 * @return The spawned task
 */
template<typename T>
Task<T> co_spawn(Task<T> task) {
    return bthread::Scheduler::Instance().Spawn(std::move(task));
}

/**
 * @brief Spawn a SafeTask coroutine for execution.
 */
template<typename T>
SafeTask<T> co_spawn(SafeTask<T> task) {
    return bthread::Scheduler::Instance().Spawn(std::move(task));
}

/**
 * @brief Spawn a detached coroutine (fire and forget).
 * The coroutine will run to completion without needing to be joined.
 */
template<typename T>
void co_spawn_detached(Task<T> task) {
    auto spawned = bthread::Scheduler::Instance().Spawn(std::move(task));
    spawned.release();
}

/**
 * @brief Spawn a detached SafeTask coroutine.
 */
template<typename T>
void co_spawn_detached(SafeTask<T> task) {
    auto spawned = bthread::Scheduler::Instance().Spawn(std::move(task));
    spawned.release();
}

} // namespace coro