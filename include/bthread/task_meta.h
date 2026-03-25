#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "bthread/platform/platform.h"

namespace bthread {

// Forward declarations
class Worker;

// bthread handle type
using bthread_t = uint64_t;

// Task state enum
enum class TaskState : uint8_t {
    READY,
    RUNNING,
    SUSPENDED,
    FINISHED
};

// Waiter state - embedded in TaskMeta (not on stack!)
struct WaiterState {
    std::atomic<WaiterState*> next{nullptr};
    std::atomic<bool> wakeup{false};
    std::atomic<bool> timed_out{false};
    int64_t deadline_us{0};
    int timer_id{0};
};

// TaskMeta - metadata for each bthread
struct TaskMeta {
    // ========== Stack Management ==========
    void* stack{nullptr};
    size_t stack_size{0};

    // ========== Context (platform-dependent) ==========
    platform::Context context{};

    // ========== State ==========
    std::atomic<TaskState> state{TaskState::READY};

    // ========== Entry Function and Result ==========
    void* (*fn)(void*){nullptr};
    void* arg{nullptr};
    void* result{nullptr};

    // ========== Reference Counting ==========
    std::atomic<int> ref_count{0};

    // ========== bthread_t Identification ==========
    uint32_t slot_index{0};
    uint32_t generation{0};

    // ========== Join Support ==========
    void* join_butex{nullptr};  // Pointer to Butex
    std::atomic<int> join_waiters{0};

    // ========== Butex Wait State ==========
    void* waiting_butex{nullptr};
    WaiterState waiter;

    // ========== Scheduling ==========
    Worker* local_worker{nullptr};

    // ========== Next pointer for queues ==========
    TaskMeta* next{nullptr};

    // Release a reference, return true if ref_count reaches 0
    bool Release() {
        return ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
};

// Utility functions for TaskMeta
namespace detail {

// Entry wrapper for bthread
void BthreadEntry(void* arg);

} // namespace detail

} // namespace bthread