// include/bthread/core/task_meta_base.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

namespace bthread {

// Forward declarations
class Worker;

// Unified task state enum - shared between bthread and coroutine
enum class TaskState : uint8_t {
    READY,      ///< Task is ready to be scheduled
    RUNNING,    ///< Task is currently executing
    SUSPENDED,  ///< Task is waiting on a synchronization primitive
    FINISHED    ///< Task has completed execution
};

// Unified task type enum - distinguishes execution model
enum class TaskType : uint8_t {
    BTHREAD,    ///< Assembly-based context switching (stackful)
    COROUTINE   ///< C++20 coroutine (stackless, compiler-managed)
};

/**
 * @brief Base class for task metadata - unified interface for both bthread and coroutine.
 *
 * This provides the common interface that allows the scheduler, queues, and
 * synchronization primitives to work with both execution models.
 *
 * Thread Safety:
 * - `state`: Atomic, safe for concurrent state transitions
 * - `next`: Atomic, used for intrusive MPSC queue linkage
 * - Other fields: Owned by a single worker thread at any time
 *
 * Design Notes:
 * - Uses virtual resume() for type-specific execution
 * - Small overhead from virtual call, but acceptable for flexibility
 * - Lifetime managed by reference counting (bthread) or coroutine handle (coroutine)
 */
struct TaskMetaBase {
    // ========== Core State ==========
    std::atomic<TaskState> state{TaskState::READY};  ///< Atomic for cross-thread transitions
    TaskType type;                                   ///< Execution model (BTHREAD or COROUTINE)

    // ========== Scheduling ==========
    std::atomic<TaskMetaBase*> next{nullptr};  ///< Intrusive queue linkage (MPSC)

    // Intrusive waiter linkage for sync primitives (Mutex/CondVar/Event)
    // Used only when state == SUSPENDED (waiting on a sync primitive)
    // Mutually exclusive with 'next' field (used for scheduling queues when state == READY)
    std::atomic<TaskMetaBase*> waiter_next{nullptr};

    Worker* owner_worker{nullptr};             ///< Worker currently running this task

    // ========== Synchronization ==========
    void* waiting_sync{nullptr};  ///< Sync primitive pointer if waiting (Mutex, CondVar, Event)

    // ========== Task Identification ==========
    uint32_t slot_index{0};    ///< Slot index in task group pool
    uint32_t generation{0};    ///< Generation for bthread_t encoding (reuse detection)

    // ========== Virtual Interface ==========
    virtual ~TaskMetaBase() = default;

    /**
     * @brief Resume the task for execution.
     * Called by the scheduler/worker when the task should run.
     * Implementation depends on task type:
     * - BTHREAD: Swap context to task's stack
     * - COROUTINE: Resume coroutine handle
     */
    virtual void resume() = 0;

    /**
     * @brief Check if the task can be resumed.
     * @return true if task is in a resumable state
     */
    bool can_resume() const {
        TaskState s = state.load(std::memory_order_acquire);
        return s == TaskState::READY || s == TaskState::RUNNING;
    }

    /**
     * @brief Check if task has completed.
     * @return true if task is finished
     */
    bool is_finished() const {
        return state.load(std::memory_order_acquire) == TaskState::FINISHED;
    }
};

} // namespace bthread