#include "bthread/butex.h"
#include "bthread/worker.h"
#include "bthread/scheduler.h"
#include "bthread/timer_thread.h"
#include "bthread/platform/platform.h"

#include <cstring>

using namespace bthread;
using namespace bthread::platform;

Butex::Butex() = default;
Butex::~Butex() = default;

void Butex::AddToTail(TaskMeta* task) {
    // Verify task is still supposed to be in queue
    if (!task->is_waiting.load(std::memory_order_relaxed)) {
        return;
    }

    WaiterNode* node = &task->waiter_node;
    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    // Exchange tail - acq_rel provides full barrier
    WaiterNode* prev = tail_.exchange(node, std::memory_order_acq_rel);

    // Check again after exchange - Wake could have cleared is_waiting
    // Use acquire to synchronize with Wake's release store
    if (!task->is_waiting.load(std::memory_order_acquire)) {
        node->claimed.store(true, std::memory_order_release);
        return;
    }

    if (prev) {
        prev->next.store(node, std::memory_order_release);
    } else {
        WaiterNode* expected = nullptr;
        head_.compare_exchange_strong(expected, node,
            std::memory_order_release, std::memory_order_relaxed);
    }
}

void Butex::AddToHead(TaskMeta* task) {
    if (!task->is_waiting.load(std::memory_order_relaxed)) {
        return;
    }

    WaiterNode* node = &task->waiter_node;
    // Don't set claimed=true - Wait() already initializes it to false

    // Use CAS loop for head insertion
    while (true) {
        WaiterNode* old_head = head_.load(std::memory_order_acquire);

        // Check is_waiting before CAS - Wake could have cleared it
        if (!task->is_waiting.load(std::memory_order_relaxed)) {
            return;
        }

        node->next.store(old_head, std::memory_order_relaxed);

        if (head_.compare_exchange_strong(old_head, node,
                std::memory_order_release, std::memory_order_relaxed)) {
            // CAS succeeded - check is_waiting one more time
            // Use acquire to synchronize with Wake's release store
            if (!task->is_waiting.load(std::memory_order_acquire)) {
                // Wake cleared is_waiting during CAS
                // Check if Wake already set state to READY
                TaskState state = task->state.load(std::memory_order_acquire);
                if (state == TaskState::READY || state == TaskState::RUNNING) {
                    // Wake already consumed this node, no need to rollback
                    // Node is orphaned in queue but marked as claimed below
                    // PopFromHead will skip it when it reaches this node
                    node->claimed.store(true, std::memory_order_release);
                    return;
                }

                // Wake cleared is_waiting but hasn't set state yet
                // Wait() will check is_waiting and state before suspending
                // Mark node as claimed so it will be skipped
                node->claimed.store(true, std::memory_order_release);

                // Roll back head to old_head (best effort)
                // Note: Another thread might have advanced head already
                WaiterNode* expected = node;
                head_.compare_exchange_strong(expected, old_head,
                    std::memory_order_release, std::memory_order_relaxed);

                return;
            }

            // Successfully inserted at head
            // If this was the first node, also update tail
            if (!old_head) {
                WaiterNode* expected = nullptr;
                tail_.compare_exchange_strong(expected, node,
                    std::memory_order_release, std::memory_order_relaxed);
            }
            return;
        }
        // CAS failed, retry
    }
}

TaskMeta* Butex::PopFromHead() {
    while (true) {
        // Load head with acquire
        WaiterNode* head = head_.load(std::memory_order_acquire);
        if (!head) return nullptr;

        // Try to claim this node
        if (head->claimed.exchange(true, std::memory_order_acq_rel)) {
            // Already claimed, skip
            head = head->next.load(std::memory_order_acquire);
            continue;
        }

        // Load next with relaxed - the CAS below provides the barrier
        WaiterNode* next = head->next.load(std::memory_order_relaxed);

        // Try to advance head - acq_rel provides synchronization for accessing head->task
        WaiterNode* expected = head;
        if (head_.compare_exchange_strong(expected, next,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Successfully claimed and advanced
            return reinterpret_cast<TaskMeta*>(
                reinterpret_cast<char*>(head) - offsetof(TaskMeta, waiter_node));
        }

        // CAS failed, reset claimed and retry
        head->claimed.store(false, std::memory_order_relaxed);
    }
}

void Butex::RemoveFromWaitQueue(TaskMeta* task) {
    // First mark as not waiting atomically
    bool expected = true;
    if (!task->is_waiting.compare_exchange_strong(expected, false,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;  // Already removed or not in queue
    }

    // Mark node as claimed so PopFromHead will skip it
    task->waiter_node.claimed.store(true, std::memory_order_release);

    // Note: We don't actually remove from linked structure
    // PopFromHead will handle it when it reaches this node
}

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
                butex->RemoveFromWaitQueue(task);
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

    // 1. Check value first
    if (value_.load(std::memory_order_acquire) != expected_value) {
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
    WaiterNode* node = &task->waiter_node;
    node->next.store(nullptr, std::memory_order_relaxed);
    node->claimed.store(false, std::memory_order_relaxed);

    // 4. Double-check value after setting is_waiting
    if (value_.load(std::memory_order_acquire) != expected_value) {
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
        AddToHead(task);
    } else {
        AddToTail(task);
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
        // Wake happened - don't suspend
        // Set READY and enqueue ourselves
        task->state.store(TaskState::READY, std::memory_order_release);
        Scheduler::Instance().EnqueueTask(task);
        task->waiting_butex = nullptr;
        task->is_waiting.store(false, std::memory_order_release);
        return 0;
    }

    // 9. Suspend - Wake will set READY and enqueue
    w->SuspendCurrent();

    // 10. Resumed
    task->waiting_butex = nullptr;

    if (task->waiter.timed_out.load(std::memory_order_acquire)) {
        return ETIMEDOUT;
    }
    return 0;
}

void Butex::Wake(int count) {
    // Wake futex waiters (pthreads)
    platform::FutexWake(&value_, count);

    int woken = 0;
    while (woken < count) {
        TaskMeta* waiter = PopFromHead();
        if (!waiter) break;

        // Clear is_waiting - task is no longer waiting
        waiter->is_waiting.store(false, std::memory_order_release);

        // Increment wake_count - signals "we saw this task"
        // Wait will detect this and handle waking itself
        waiter->wake_count.fetch_add(1, std::memory_order_release);

        // Cancel pending timeout
        if (waiter->waiter.timer_id != 0) {
            Scheduler::Instance().GetTimerThread()->Cancel(waiter->waiter.timer_id);
        }

        // Check if task is SUSPENDED
        TaskState state = waiter->state.load(std::memory_order_acquire);
        if (state == TaskState::SUSPENDED) {
            // Task is suspended, wake it up
            waiter->state.store(TaskState::READY, std::memory_order_release);
            Scheduler::Instance().EnqueueTask(waiter);
        }
        // If RUNNING or READY, Wait will handle it

        ++woken;
    }
}