// include/coro/scheduler.h
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <thread>
#include "coro/meta.h"
#include "coro/coroutine.h"

namespace coro {

/**
 * @brief Coroutine scheduler implementing M:N threading model.
 *
 * Thread Safety:
 * - Init(): Thread-safe via std::call_once. Can be called from any thread.
 * - Shutdown(): Thread-safe. Can be called from any thread.
 * - Spawn(): Thread-safe. Can be called from any thread concurrently.
 * - EnqueueCoroutine(): Thread-safe. Can be called from any thread.
 * - AllocMeta(): Thread-safe via meta_mutex_.
 * - FreeMeta(): Thread-safe via meta_mutex_.
 *
 * Usage:
 * - Call Init() before spawning tasks, or Spawn() will auto-initialize.
 * - Call Shutdown() to stop worker threads (optional - destructor also calls it).
 */
class CoroutineScheduler {
public:
    static CoroutineScheduler& Instance();

    void Init();
    void Shutdown();

    bool running() const {
        return running_.load(std::memory_order_acquire);
    }

    CoroutineQueue& global_queue() { return global_queue_; }

    /**
     * @brief Spawn a task for execution by the scheduler.
     * Thread-safe: Can be called from any thread concurrently.
     * Auto-initializes the scheduler if not already initialized.
     */
    template<typename T>
    Task<T> Spawn(Task<T> task) {
        // Auto-initialize if not already initialized
        if (!initialized_.load(std::memory_order_acquire)) {
            Init();
        }

        CoroutineMeta* meta = AllocMeta();
        meta->handle = task.handle();
        meta->state.store(CoroutineMeta::READY, std::memory_order_release);

        // Store CoroutineMeta in promise
        task.handle().promise().set_meta(meta);

        EnqueueCoroutine(meta);
        return std::move(task);
    }

    /**
     * @brief Spawn a SafeTask for execution by the scheduler.
     * Thread-safe: Can be called from any thread concurrently.
     * Auto-initializes the scheduler if not already initialized.
     */
    template<typename T>
    SafeTask<T> Spawn(SafeTask<T> task) {
        // Auto-initialize if not already initialized
        if (!initialized_.load(std::memory_order_acquire)) {
            Init();
        }

        CoroutineMeta* meta = AllocMeta();
        meta->handle = task.handle();
        meta->state.store(CoroutineMeta::READY, std::memory_order_release);

        // Store CoroutineMeta in promise
        task.handle().promise().set_meta(meta);

        EnqueueCoroutine(meta);
        return std::move(task);
    }

    void EnqueueCoroutine(CoroutineMeta* meta);
    CoroutineMeta* AllocMeta();
    void FreeMeta(CoroutineMeta* meta);

    // Get worker count
    size_t worker_count() const { return workers_.size(); }

private:
    CoroutineScheduler() = default;
    ~CoroutineScheduler();

    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::once_flag init_once_;

    std::vector<std::thread> workers_;
    CoroutineQueue global_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::vector<std::unique_ptr<CoroutineMeta>> meta_pool_;
    std::mutex meta_mutex_;

    void InitMetaPool(size_t count);
    void StartCoroutineWorkers(int count);
    void CoroutineWorkerLoop();
};

// co_spawn function
template<typename T>
Task<T> co_spawn(Task<T> task) {
    return CoroutineScheduler::Instance().Spawn(std::move(task));
}

// co_spawn for SafeTask
template<typename T>
SafeTask<T> co_spawn(SafeTask<T> task) {
    return CoroutineScheduler::Instance().Spawn(std::move(task));
}

// co_spawn_detached - fire and forget
template<typename T>
void co_spawn_detached(Task<T> task) {
    auto spawned = CoroutineScheduler::Instance().Spawn(std::move(task));
    // Release ownership of the handle - the scheduler now owns the coroutine
    // Without this, the returned Task's destructor would destroy the coroutine
    spawned.release();
}

// co_spawn_detached for SafeTask
template<typename T>
void co_spawn_detached(SafeTask<T> task) {
    auto spawned = CoroutineScheduler::Instance().Spawn(std::move(task));
    spawned.release();
}

} // namespace coro