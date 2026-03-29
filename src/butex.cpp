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

void Butex::RemoveFromWaitQueue(TaskMeta* waiter) {
    TaskMeta* prev = nullptr;
    TaskMeta* curr = waiters_.load(std::memory_order_acquire);

    while (curr) {
        if (curr == waiter) {
            TaskMeta* next = waiter->waiter.next.load(std::memory_order_acquire);

            if (prev) {
                prev->waiter.next.store(next, std::memory_order_release);
            } else {
                waiters_.compare_exchange_strong(curr, next,
                    std::memory_order_acq_rel, std::memory_order_acquire);
            }
            return;
        }
        prev = curr;
        curr = curr->waiter.next.load(std::memory_order_acquire);
    }
}

void Butex::TimeoutCallback(void* arg) {
    TaskMeta* task = static_cast<TaskMeta*>(arg);
    WaiterState& ws = task->waiter;

    // Try to mark as timed out
    bool expected = false;
    if (ws.wakeup.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        ws.timed_out.store(true, std::memory_order_release);

        // Only re-queue if task is already suspended
        if (task->state.load(std::memory_order_acquire) == TaskState::SUSPENDED) {
            task->state.store(TaskState::READY, std::memory_order_release);
            Scheduler::Instance().EnqueueTask(task);
        }
        // If not suspended, the task is still running and will see wakeup=true
        // in its double-check and return without suspending
    }
}

int Butex::Wait(int expected_value, const platform::timespec* timeout) {
    Worker* w = Worker::Current();
    if (w == nullptr) {
        // Called from pthread, use futex directly
        return platform::FutexWait(&value_, expected_value, timeout);
    }

    TaskMeta* task = w->current_task();

    // 1. Check value first
    if (value_.load(std::memory_order_acquire) != expected_value) {
        return 0;
    }

    // 2. Initialize wait state in TaskMeta (NOT on stack!)
    WaiterState& ws = task->waiter;
    ws.wakeup.store(false, std::memory_order_relaxed);
    ws.timed_out.store(false, std::memory_order_relaxed);
    ws.deadline_us = 0;
    ws.timer_id = 0;

    // 3. Add to wait queue (push to head)
    TaskMeta* old_head = waiters_.load(std::memory_order_relaxed);
    do {
        ws.next.store(old_head, std::memory_order_relaxed);
    } while (!waiters_.compare_exchange_weak(old_head, task,
            std::memory_order_release, std::memory_order_relaxed));

    // 4. Double-check value
    if (value_.load(std::memory_order_acquire) != expected_value) {
        // Remove from queue
        RemoveFromWaitQueue(task);
        return 0;
    }

    // 4.5 Check if already woken by Wake (race condition: Wake stole us before we suspended)
    if (ws.wakeup.load(std::memory_order_acquire)) {
        // Already woken, remove from queue and return
        RemoveFromWaitQueue(task);
        return 0;
    }

    // 5. Set up timeout
    if (timeout) {
        ws.deadline_us = platform::GetTimeOfDayUs() +
            (static_cast<int64_t>(timeout->tv_sec) * 1000000 +
             timeout->tv_nsec / 1000);

        platform::timespec ts = *timeout;
        ws.timer_id = Scheduler::Instance().GetTimerThread()->Schedule(
            TimeoutCallback, task, &ts);
    }

    // 6. Record which butex we're waiting on
    task->waiting_butex = this;

    // 7. Set state to SUSPENDED before final wakeup check
    // This ensures Wake can see SUSPENDED state and properly wake us
    task->state.store(TaskState::SUSPENDED, std::memory_order_release);

    // 7.5 Final check for wakeup after setting SUSPENDED state
    // This prevents the race where Wake sets wakeup but doesn't see SUSPENDED yet
    if (ws.wakeup.load(std::memory_order_acquire)) {
        // Already woken, restore state and return
        task->state.store(TaskState::READY, std::memory_order_release);
        task->waiting_butex = nullptr;
        return 0;
    }

    // 8. Final check: ensure state is still SUSPENDED before suspending
    // If Wake changed it to READY, we should not suspend
    if (task->state.load(std::memory_order_acquire) != TaskState::SUSPENDED) {
        // State changed by Wake, don't suspend
        task->waiting_butex = nullptr;
        return 0;
    }

    // 9. Suspend
    w->SuspendCurrent();

    // 10. Resumed - check result
    task->waiting_butex = nullptr;

    if (ws.timed_out.load(std::memory_order_acquire)) {
        return ETIMEDOUT;
    }
    return 0;
}

void Butex::Wake(int count) {
    // Wake futex waiters (pthreads waiting via FutexWait)
    // Note: The caller should have already changed the value (generation)
    // before calling Wake, so pthread waiters can detect the change
    platform::FutexWake(&value_, count);

    int woken = 0;

    while (woken < count) {
        // Pop from head
        TaskMeta* waiter = waiters_.load(std::memory_order_acquire);
        if (!waiter) break;

        // Try to atomically claim this waiter
        TaskMeta* next = waiter->waiter.next.load(std::memory_order_relaxed);
        if (waiters_.compare_exchange_weak(waiter, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {

            WaiterState& ws = waiter->waiter;

            // Check if already woken/timed out
            bool expected = false;
            if (ws.wakeup.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                ++woken;

                // Cancel pending timeout
                if (ws.timer_id != 0) {
                    Scheduler::Instance().GetTimerThread()->Cancel(ws.timer_id);
                }

                // Only re-queue if task is already suspended
                // If not suspended yet, the task will check wakeup and return without suspending
                if (waiter->state.load(std::memory_order_acquire) == TaskState::SUSPENDED) {
                    waiter->state.store(TaskState::READY, std::memory_order_release);
                    Scheduler::Instance().EnqueueTask(waiter);
                }
                // If not suspended, the task is still running and will see wakeup=true
                // in its double-check and return without suspending
            }
        }
    }
}