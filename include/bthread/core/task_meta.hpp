// include/bthread/core/task_meta.hpp
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "bthread/core/task_meta_base.hpp"
#include "bthread/queue/mpmc_queue.hpp"
#include "bthread/platform/platform.h"

namespace bthread {

// Forward declarations
class Worker;
struct TaskMeta;  // Forward declare for WaiterState

// bthread handle type (legacy compatibility)
using bthread_t = uint64_t;

// Waiter state - embedded in TaskMeta (not on stack!)
// Supports doubly-linked list for FIFO/LIFO flexibility
struct WaiterState {
    std::atomic<TaskMeta*> next{nullptr};
    std::atomic<TaskMeta*> prev{nullptr};  // For doubly-linked list
    // std::atomic<bool> in_queue{false};     // REMOVED - replaced by is_waiting
    std::atomic<bool> wakeup{false};
    std::atomic<bool> timed_out{false};
    int64_t deadline_us{0};
    int timer_id{0};
};

/**
 * @brief Task metadata for bthread (stackful user-space thread).
 * Inherits from TaskMetaBase and adds bthread-specific fields.
 *
 * bthread uses assembly-based context switching with a dedicated stack,
 * unlike coroutines which use compiler-managed context.
 *
 * Memory layout optimized for cache locality:
 * - Hot fields (context switching, scheduling) grouped first
 * - Synchronization fields grouped second
 * - Less frequently accessed fields grouped last
 */
struct TaskMeta : TaskMetaBase {
    // Constructor - set type to BTHREAD
    TaskMeta() : TaskMetaBase() {
        type = TaskType::BTHREAD;
    }

    // ========== Group 1: Context Switching (HOT - first cache line) ==========
    void* stack{nullptr};
    size_t stack_size{0};
    platform::Context context{};

    // ========== Group 2: Scheduling State (HOT) ==========
    // Note: state is inherited from TaskMetaBase (atomic<TaskState>)
    std::atomic<bool> is_waiting{false};  // Prevents ABA, replaces in_queue
    std::atomic<int> wake_count{0};       // Number of Wake operations seen

    // ========== Group 3: Synchronization (WARM) ==========
    void* waiting_butex{nullptr};  ///< Butex pointer if waiting on one
    WaiterState waiter;
    MpmcNode mpmc_node;  // Inline node for MPMC queue, no dynamic alloc

    // ========== Group 4: Entry Function and Result (COLD) ==========
    void* (*fn)(void*){nullptr};
    void* arg{nullptr};
    void* result{nullptr};

    // ========== Group 5: Join Support (COLD) ==========
    void* join_butex{nullptr};  ///< Pointer to Butex
    std::atomic<int> join_waiters{0};
    std::atomic<int> ref_count{0};

    // ========== Group 6: Other (COLD) ==========
    bool uses_xmm{false};  // True if task uses SIMD (xmm6-xmm15)
    Worker* local_worker{nullptr};  ///< Worker affinity for task execution
    TaskMeta* legacy_next{nullptr};  ///< Used for bthread-specific linked lists

    // ========== Resume Implementation ==========
    void resume() override;

    // ========== Reference Management ==========
    /**
     * @brief Release a reference, return true if ref_count reaches 0.
     * Used for join/detach semantics.
     */
    bool Release() {
        return ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
};

// Utility functions for TaskMeta
namespace detail {

// Entry wrapper for bthread - called from assembly context
void BthreadEntry(void* arg);

} // namespace detail

} // namespace bthread