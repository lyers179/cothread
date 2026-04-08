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
#include "bthread/core/scheduler.hpp"  // Include full definition for Spawn()

namespace coro {

/**
 * @brief Coroutine scheduler - now delegates to the unified bthread::Scheduler.
 *
 * This class is kept for backward compatibility. New code should use
 * bthread::Scheduler::Instance().Spawn() or coro::co_spawn() directly.
 *
 * Thread Safety:
 * - All methods are thread-safe and delegate to the unified scheduler.
 *
 * Deprecated: Use bthread::Scheduler directly for new code.
 */
class CoroutineScheduler {
public:
    /// Get instance - now returns a wrapper that delegates to bthread::Scheduler
    [[deprecated("Use bthread::Scheduler::Instance() instead")]]
    static CoroutineScheduler& Instance();

    /// Initialize the unified scheduler
    void Init();

    /// Shutdown the unified scheduler
    void Shutdown();

    /// Check if running
    bool running() const;

    /// Get global queue (deprecated - use bthread::Scheduler::global_queue())
    bthread::TaskQueue& global_queue();

    /**
     * @brief Spawn a task for execution by the unified scheduler.
     * Thread-safe: Can be called from any thread concurrently.
     * Auto-initializes the scheduler if not already initialized.
     */
    template<typename T>
    Task<T> Spawn(Task<T> task);

    /**
     * @brief Spawn a SafeTask for execution by the unified scheduler.
     * Thread-safe: Can be called from any thread concurrently.
     */
    template<typename T>
    SafeTask<T> Spawn(SafeTask<T> task);

    /// Enqueue coroutine - delegates to unified scheduler
    void EnqueueCoroutine(CoroutineMeta* meta);

    /// Allocate coroutine metadata
    CoroutineMeta* AllocMeta();

    /// Free coroutine metadata
    void FreeMeta(CoroutineMeta* meta);

    /// Get worker count
    size_t worker_count() const;

private:
    CoroutineScheduler() = default;
    ~CoroutineScheduler();

    std::vector<std::unique_ptr<CoroutineMeta>> meta_pool_;
    std::mutex meta_mutex_;
};

} // namespace coro