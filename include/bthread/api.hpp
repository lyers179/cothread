// include/bthread/api.hpp
#pragma once

#include <type_traits>
#include <functional>
#include <memory>

#include "bthread/core/task.hpp"
#include "bthread/core/task_meta_base.hpp"
#include "bthread/core/scheduler.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/core/task_group.hpp"
#include "bthread/platform/platform.h"

// Forward declarations for coroutine support
namespace coro {
template<typename T> class Task;
template<typename T> class SafeTask;
}

namespace bthread {

// ========== Spawn Functions ==========

/**
 * @brief Spawn a function as a bthread task.
 *
 * Creates a bthread (assembly-based context switching) that runs the given function.
 *
 * @tparam F Callable type
 * @tparam Args Argument types
 * @param func The function to run
 * @param args Arguments to pass to the function
 * @return Task handle for the spawned task
 */
template<typename F, typename... Args>
Task spawn(F&& func, Args&&... args) {
    static_assert(std::is_invocable_v<F, Args...>,
        "Function must be callable with the given arguments");

    // Determine return type
    using ReturnType = std::invoke_result_t<F, Args...>;

    // Create a wrapper that captures the function and arguments
    auto wrapper = [f = std::forward<F>(func),
                    args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable -> ReturnType {
        return std::apply(f, args_tuple);
    };

    // Get scheduler and initialize if needed
    Scheduler& sched = Scheduler::Instance();
    if (!sched.initialized()) {
        sched.Init();
    }

    // Create TaskMeta
    TaskMeta* meta = GetTaskGroup().AllocTaskMeta();
    if (!meta) {
        throw std::runtime_error("Failed to allocate TaskMeta");
    }

    // Set up the task
    auto* wrapper_ptr = new std::function<ReturnType()>(std::move(wrapper));

    meta->fn = +[](void* arg) -> void* {
        auto* w_ptr = static_cast<std::function<ReturnType()>*>(arg);
        (*w_ptr)();
        delete w_ptr;
        return nullptr;
    };

    meta->arg = wrapper_ptr;
    meta->state.store(TaskState::READY, std::memory_order_release);

    // Allocate stack
    if (!meta->stack) {
        constexpr size_t DEFAULT_STACK_SIZE = 1024 * 1024;  // 1MB
        meta->stack = platform::AllocateStack(DEFAULT_STACK_SIZE);
        meta->stack_size = DEFAULT_STACK_SIZE;
    }

    // Set up context
    platform::MakeContext(&meta->context, meta->stack, meta->stack_size,
                          detail::BthreadEntry, meta);

    // Encode bthread_t
    bthread_t tid = GetTaskGroup().EncodeId(meta->slot_index, meta->generation);

    // Submit to scheduler
    sched.EnqueueTask(meta);

    // Create Task handle
    Task task;
    task.set_meta(meta);
    return task;
}

/**
 * @brief Explicitly spawn a function as a bthread (assembly context).
 *
 * Same as spawn() but explicitly uses bthread execution model.
 */
template<typename F, typename... Args>
Task spawn_bthread(F&& func, Args&&... args) {
    return spawn(std::forward<F>(func), std::forward<Args>(args)...);
}

/**
 * @brief Spawn a coroutine task.
 *
 * Takes a coroutine Task or SafeTask and schedules it for execution.
 *
 * @tparam T The coroutine return type
 * @param task The coroutine task to spawn
 * @return The spawned task handle
 */
template<typename T>
Task spawn_coro(coro::Task<T> task) {
    // Get scheduler and initialize if needed
    Scheduler& sched = Scheduler::Instance();
    if (!sched.initialized()) {
        sched.Init();
    }

    auto spawned = sched.Spawn(std::move(task));

    // Create Task handle
    Task result;
    result.set_meta(spawned.handle().promise().meta());
    spawned.release();  // Don't destroy the coroutine
    return result;
}

/**
 * @brief Spawn a SafeTask coroutine.
 */
template<typename T>
Task spawn_coro(coro::SafeTask<T> task) {
    Scheduler& sched = Scheduler::Instance();
    if (!sched.initialized()) {
        sched.Init();
    }

    auto spawned = sched.Spawn(std::move(task));

    Task result;
    result.set_meta(spawned.handle().promise().meta());
    spawned.release();
    return result;
}

/**
 * @brief Spawn a detached task (fire and forget).
 *
 * The task runs independently and cleans up when done.
 * The returned Task handle is invalid after this call.
 */
template<typename F, typename... Args>
void spawn_detached(F&& func, Args&&... args) {
    Task task = spawn(std::forward<F>(func), std::forward<Args>(args)...);
    task.detach();
}

// ========== Utility Functions ==========

/**
 * @brief Get the current task ID.
 * @return The ID of the currently executing task, or 0 if not in a task
 */
inline uint64_t current_task_id() {
    Worker* w = Worker::Current();
    if (!w) return 0;

    TaskMetaBase* task = w->current_task();
    if (!task) return 0;

    return (static_cast<uint64_t>(task->generation) << 32) | task->slot_index;
}

/**
 * @brief Yield the current task.
 * Allows other tasks to run before resuming this one.
 */
inline void yield() {
    Worker* w = Worker::Current();
    if (w) {
        w->YieldCurrent();
    } else {
        std::this_thread::yield();
    }
}

/**
 * @brief Get the number of worker threads.
 */
inline int worker_count() {
    return Scheduler::Instance().worker_count();
}

/**
 * @brief Set the number of worker threads.
 * Must be called before any tasks are spawned.
 */
inline void set_worker_count(int count) {
    Scheduler::Instance().set_worker_count(count);
}

/**
 * @brief Initialize the scheduler.
 * Automatically called on first spawn, but can be called explicitly.
 */
inline void init() {
    Scheduler::Instance().Init();
}

/**
 * @brief Shutdown the scheduler.
 * Waits for all workers to finish and cleans up resources.
 */
inline void shutdown() {
    Scheduler::Instance().Shutdown();
}

} // namespace bthread