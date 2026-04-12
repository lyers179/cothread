#include "bthread/sync/butex.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/core/worker.hpp"
#include "bthread/core/scheduler.hpp"
#include "bthread/detail/timer_thread.hpp"
#include "bthread/platform/platform.h"

#include <cstring>

namespace bthread {

using namespace bthread::platform;

// Offset of mpmc_node in TaskMeta for container_of conversion
static constexpr size_t kMpmcNodeOffset = offsetof(TaskMeta, mpmc_node);

// Helper to convert MpmcNode* to TaskMeta*
static inline TaskMeta* NodeToTaskMeta(MpmcNode* node) {
    return NodeToParent<TaskMeta>(node, kMpmcNodeOffset);
}

Butex::Butex() = default;
Butex::~Butex() = default;

void Butex::TimeoutCallback(void* arg) {
    TaskMeta* task = static_cast<TaskMeta*>(arg);
    WaiterState& ws = task->waiter;

    // Try to mark as timed out
    bool expected = false;
    if (ws.wakeup.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        ws.timed_out.store(true, std::memory_order_release);

        // Check if task is SUSPENDED - only then re-queue it
        TaskState state = task->state.load(std::memory_order_acquire);
        if (state == TaskState::SUSPENDED) {
            // Remove from queue first
            Butex* butex = static_cast<Butex*>(task->waiting_butex);
            if (butex) {
                butex->queue().Remove(&task->mpmc_node, &task->is_waiting);
            }

            task->state.store(TaskState::READY, std::memory_order_release);
            Scheduler::Instance().EnqueueTask(task);
        }
        // If not SUSPENDED, the task is still preparing to suspend
        // It will check wakeup and return without suspending
    }
}

int Butex::Wait(int expected_value, const platform::timespec* timeout, bool prepend) {
    Worker* w = Worker::Current();
    if (w == nullptr) {
        // Called from pthread, use futex directly
        return platform::FutexWait(&value_, expected_value, timeout);
    }

    // Get current task and cast to TaskMeta (Butex only works with bthread)
    TaskMetaBase* base_task = w->current_task();
    if (!base_task || base_task->type != TaskType::BTHREAD) {
        // Not a bthread, use futex
        return platform::FutexWait(&value_, expected_value, timeout);
    }

    TaskMeta* task = static_cast<TaskMeta*>(base_task);

    // Check if scheduler is shutting down - return error immediately
    if (!Scheduler::Instance().running()) {
        return ECANCELED;  // Operation cancelled due to shutdown
    }

    // 1. Check value first - use relaxed for quick check, will re-check with acquire
    int val = value_.load(std::memory_order_relaxed);
    if (val != expected_value) {
        return 0;
    }

    // 2. Mark as "about to enter queue" - prevent concurrent Wake
    bool expected = false;
    if (!task->is_waiting.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // Already waiting - task is being consumed, return immediately
        return 0;
    }

    // 3. Prepare waiter node
    MpmcNode* node = &task->mpmc_node;
    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    // 4. Double-check value after setting is_waiting - use acquire for synchronization
    val = value_.load(std::memory_order_acquire);
    if (val != expected_value) {
        // Value changed, remove ourselves
        task->is_waiting.store(false, std::memory_order_release);
        return 0;
    }

    // 5. Set up timeout
    if (timeout) {
        platform::timespec ts = *timeout;
        task->waiter.deadline_us = platform::GetTimeOfDayUs() +
            (static_cast<int64_t>(ts.tv_sec) * 1000000 +
             ts.tv_nsec / 1000);
        task->waiter.timer_id = Scheduler::Instance().GetTimerThread()->Schedule(
            TimeoutCallback, task, &ts);
    }

    // 6. Record which butex we're waiting on
    task->waiting_butex = this;

    // 7. Add to queue (lock-free MPSC)
    if (prepend) {
        // Cast state to uint8_t* since TaskState is enum class : uint8_t
        queue_.AddToHead(node, &task->is_waiting,
            reinterpret_cast<std::atomic<uint8_t>*>(&task->state));
    } else {
        queue_.AddToTail(node, &task->is_waiting);
    }

    // 7.5. Re-check value AFTER adding to queue to catch concurrent Wake
    // Wake might have changed value between our initial check and adding to queue
    val = value_.load(std::memory_order_acquire);
    if (val != expected_value) {
        // Value changed, remove ourselves from queue
        // Mark as claimed so PopFromHead will skip us
        node->claimed.store(true, std::memory_order_release);
        task->is_waiting.store(false, std::memory_order_release);
        task->waiting_butex = nullptr;
        return 0;
    }

    // 8. Try to set state to SUSPENDED using CAS
    // Save wake_count before CAS to detect Wake during/after CAS
    int saved_wake_count = task->wake_count.load(std::memory_order_acquire);

    TaskState expected_state = TaskState::RUNNING;
    if (!task->state.compare_exchange_strong(expected_state, TaskState::SUSPENDED,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        // CAS failed: Wake already set state to READY
        // We're woken, return immediately
        task->waiting_butex = nullptr;
        task->is_waiting.store(false, std::memory_order_release);
        return 0;
    }

    // CAS succeeded: state is now SUSPENDED
    // Check if Wake happened during CAS
    int current_wake_count = task->wake_count.load(std::memory_order_acquire);
    if (current_wake_count != saved_wake_count) {
        // Wake happened - try to take ownership via CAS
        // Only one side (Wake or Wait) should enqueue the task
        TaskState expected = TaskState::SUSPENDED;
        if (task->state.compare_exchange_strong(expected, TaskState::READY,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // We won the race - Wake will see state != SUSPENDED and skip us
            Scheduler::Instance().EnqueueTask(task);
        }
        // If CAS failed, Wake already set READY and will enqueue us
        task->waiting_butex = nullptr;
        task->is_waiting.store(false, std::memory_order_release);
        return 0;
    }

    // 9. Suspend - Wake will set READY and enqueue
    w->SuspendCurrent();

    // 10. Resumed
    task->waiting_butex = nullptr;

    // Check if we were woken due to shutdown
    if (!Scheduler::Instance().running()) {
        return ECANCELED;
    }

    if (task->waiter.timed_out.load(std::memory_order_acquire)) {
        return ETIMEDOUT;
    }
    return 0;
}

void Butex::Wake(int count) {
    // Wake futex waiters (pthreads)
    platform::FutexWake(&value_, count);

    // Use a fixed-size stack array to avoid dynamic allocation
    // Most Wake calls are for small counts (1 or a few waiters)
    constexpr int BATCH_SIZE = 16;
    MpmcNode* nodes[BATCH_SIZE];

    int total_woken = 0;
    while (total_woken < count) {
        // Batch pop to reduce CAS overhead
        int batch_count = queue_.PopMultipleFromHead(nodes, std::min(count - total_woken, BATCH_SIZE));
        if (batch_count == 0) break;

        for (int i = 0; i < batch_count; ++i) {
            // Convert MpmcNode* to TaskMeta*
            TaskMeta* waiter = NodeToTaskMeta(nodes[i]);

            // Mark as not waiting - this tells Wait that we're handling it
            waiter->is_waiting.store(false, std::memory_order_release);

            // Increment wake_count with release ordering
            waiter->wake_count.fetch_add(1, std::memory_order_release);

            // Cancel pending timeout
            if (waiter->waiter.timer_id != 0) {
                Scheduler::Instance().GetTimerThread()->Cancel(waiter->waiter.timer_id);
            }

            // Check if task is SUSPENDED - use acquire to see latest state
            TaskState state = waiter->state.load(std::memory_order_acquire);
            if (state == TaskState::SUSPENDED) {
                // Direct store - Wait uses CAS for protection
                // Wake increments wake_count first, Wait CAS will fail if Wake already stored
                waiter->state.store(TaskState::READY, std::memory_order_release);
                Scheduler::Instance().EnqueueTask(waiter);
            }
            // If RUNNING or READY, Wait will handle it
        }

        total_woken += batch_count;
    }
}

} // namespace bthread