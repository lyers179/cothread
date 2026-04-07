// include/bthread/task_meta.h
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "bthread/core/task_meta_base.hpp"
#include "bthread/platform/platform.h"

namespace bthread {

// Forward declarations
class Worker;
struct TaskMeta;  // Forward declare for WaiterState

// WaiterNode - lock-free queue node (inline in TaskMeta)
struct WaiterNode {
    std::atomic<WaiterNode*> next{nullptr};
    std::atomic<bool> claimed{false};  // Prevents double consumption
};

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
 */
struct TaskMeta : TaskMetaBase {
    // Constructor - set type to BTHREAD
    TaskMeta() : TaskMetaBase() {
        type = TaskType::BTHREAD;
    }

    // ========== Stack Management (bthread-specific) ==========
    void* stack{nullptr};
    size_t stack_size{0};

    // ========== Context (platform-dependent, bthread-specific) ==========
    platform::Context context{};

    // ========== Entry Function and Result (bthread-specific) ==========
    void* (*fn)(void*){nullptr};
    void* arg{nullptr};
    void* result{nullptr};

    // ========== Reference Counting (bthread-specific) ==========
    std::atomic<int> ref_count{0};

    // ========== Join Support (bthread-specific) ==========
    void* join_butex{nullptr};  ///< Pointer to Butex
    std::atomic<int> join_waiters{0};

    // ========== Butex Wait State (bthread-specific) ==========
    void* waiting_butex{nullptr};  ///< Butex pointer if waiting on one
    WaiterState waiter;

    // ========== Lock-Free Wait Queue ==========
    std::atomic<bool> is_waiting{false};  // Prevents ABA, replaces in_queue
    WaiterNode waiter_node;               // Inline node, no dynamic alloc

    // ========== XMM Lazy Saving ==========
    bool uses_xmm{false};  // True if task uses SIMD (xmm6-xmm15)

    // ========== Worker Affinity (bthread-specific) ==========
    Worker* local_worker{nullptr};  ///< Worker affinity for task execution

    // ========== Legacy Next Pointer (bthread-specific) ==========
    /// Used for bthread-specific linked lists (e.g., Butex wait queue)
    /// Note: TaskMetaBase::next is for scheduler queue, this is for sync primitives
    TaskMeta* legacy_next{nullptr};

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