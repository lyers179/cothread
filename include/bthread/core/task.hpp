// include/bthread/core/task.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <coroutine>

#include "bthread/core/task_meta_base.hpp"

// Forward declarations
namespace bthread {
class Scheduler;
}

namespace coro {
template<typename T> class Task;
template<typename T> class SafeTask;
struct CoroutineMeta;
}

namespace bthread {

/**
 * @brief Unified task handle - works for both bthread and coroutine.
 *
 * This class provides a type-erased handle to a task that can represent
 * either a bthread (assembly-based context switching) or a coroutine
 * (C++20 compiler-managed).
 *
 * Thread Safety:
 * - All methods are thread-safe unless otherwise noted
 * - join() and detach() should only be called once
 *
 * Usage:
 * ```cpp
 * // Spawn a bthread
 * bthread::Task t1 = bthread::spawn([]{ return 42; });
 *
 * // Spawn a coroutine
 * bthread::Task t2 = bthread::spawn([]() -> coro::Task<int> { co_return 42; });
 *
 * // Wait for completion
 * t1.join();
 *
 * // Get result (for coroutine tasks)
 * auto result = t2.get<int>();
 * ```
 */
class Task {
public:
    /// Default constructor - creates an empty task handle
    Task() = default;

    /// Move constructor
    Task(Task&& other) noexcept
        : meta_(other.meta_), result_ptr_(std::move(other.result_ptr_)) {
        other.meta_ = nullptr;
        other.result_ptr_ = nullptr;
    }

    /// Move assignment
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            // Release current task if any
            if (meta_ && meta_->type == TaskType::COROUTINE) {
                // Coroutine cleanup is handled by coroutine framework
            }
            meta_ = other.meta_;
            result_ptr_ = std::move(other.result_ptr_);
            other.meta_ = nullptr;
            other.result_ptr_ = nullptr;
        }
        return *this;
    }

    // Disable copy
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /// Destructor
    ~Task() = default;

    // ========== Query Methods ==========

    /**
     * @brief Check if the task handle is valid.
     * @return true if the handle refers to a task
     */
    bool valid() const {
        return meta_ != nullptr;
    }

    /**
     * @brief Check if the task has completed.
     * @return true if the task is finished
     */
    bool is_done() const {
        if (!meta_) return true;
        return meta_->state.load(std::memory_order_acquire) == TaskState::FINISHED;
    }

    /**
     * @brief Check if the task is currently running.
     * @return true if the task is in RUNNING state
     */
    bool is_running() const {
        if (!meta_) return false;
        return meta_->state.load(std::memory_order_acquire) == TaskState::RUNNING;
    }

    /**
     * @brief Get the task ID.
     * @return Unique identifier for this task
     */
    uint64_t id() const {
        if (!meta_) return 0;
        return (static_cast<uint64_t>(meta_->generation) << 32) | meta_->slot_index;
    }

    /**
     * @brief Get the task type.
     * @return TaskType::BTHREAD or TaskType::COROUTINE
     */
    TaskType type() const {
        if (!meta_) return TaskType::BTHREAD;
        return meta_->type;
    }

    // ========== Operations ==========

    /**
     * @brief Wait for the task to complete.
     * Blocks the current thread/task until this task finishes.
     */
    void join();

    /**
     * @brief Detach the task.
     * The task will continue running and clean up automatically when done.
     */
    void detach();

    /**
     * @brief Get the result of a coroutine task.
     * Must only be called after the task is done and for coroutine tasks.
     * @tparam T The result type
     * @return The result value
     */
    template<typename T>
    T get() {
        if (!meta_) {
            throw std::runtime_error("Task has no handle");
        }
        if (!is_done()) {
            throw std::runtime_error("Task not completed");
        }
        if (meta_->type != TaskType::COROUTINE) {
            throw std::runtime_error("get() only works for coroutine tasks");
        }
        // This is a placeholder - actual implementation depends on coroutine framework
        throw std::runtime_error("Not implemented");
    }

    // ========== Internal Methods ==========

    /// Set the task metadata (internal use)
    void set_meta(TaskMetaBase* meta) { meta_ = meta; }

    /// Get the task metadata (internal use)
    TaskMetaBase* get_meta() const { return meta_; }

private:
    TaskMetaBase* meta_{nullptr};
    std::shared_ptr<void> result_ptr_;
};

/**
 * @brief Result type for tasks that return a value.
 *
 * Used internally to store the result of a task execution.
 */
template<typename T>
struct TaskResult {
    T value;
    std::exception_ptr exception;

    bool has_exception() const { return exception != nullptr; }

    T get() {
        if (exception) {
            std::rethrow_exception(exception);
        }
        return std::move(value);
    }
};

// Specialization for void
template<>
struct TaskResult<void> {
    std::exception_ptr exception;

    bool has_exception() const { return exception != nullptr; }

    void get() {
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
};

} // namespace bthread