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

void Butex::AddToHead(TaskMeta* waiter) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    waiter->waiter.next.store(head_, std::memory_order_relaxed);
    waiter->waiter.prev.store(nullptr, std::memory_order_relaxed);
    if (head_) {
        head_->waiter.prev.store(waiter, std::memory_order_relaxed);
    } else {
        tail_ = waiter;  // Queue was empty
    }
    head_ = waiter;
    waiter->waiter.in_queue.store(true, std::memory_order_release);
}

void Butex::AddToTail(TaskMeta* waiter) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    waiter->waiter.next.store(nullptr, std::memory_order_relaxed);
    waiter->waiter.prev.store(tail_, std::memory_order_relaxed);
    if (tail_) {
        tail_->waiter.next.store(waiter, std::memory_order_relaxed);
    } else {
        head_ = waiter;  // Queue was empty
    }
    tail_ = waiter;
    waiter->waiter.in_queue.store(true, std::memory_order_release);
}

void Butex::RemoveFromWaitQueue(TaskMeta* waiter) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Check if still in queue (may have been removed by PopFromHead)
    if (!waiter->waiter.in_queue.load(std::memory_order_acquire)) {
        return;  // Already removed
    }

    TaskMeta* prev = waiter->waiter.prev.load(std::memory_order_relaxed);
    TaskMeta* next = waiter->waiter.next.load(std::memory_order_relaxed);

    if (prev) {
        prev->waiter.next.store(next, std::memory_order_relaxed);
    } else {
        head_ = next;
    }

    if (next) {
        next->waiter.prev.store(prev, std::memory_order_relaxed);
    } else {
        tail_ = prev;
    }

    // Clear links and mark as not in queue
    waiter->waiter.next.store(nullptr, std::memory_order_relaxed);
    waiter->waiter.prev.store(nullptr, std::memory_order_relaxed);
    waiter->waiter.in_queue.store(false, std::memory_order_release);
}

TaskMeta* Butex::PopFromHead() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!head_) {
        return nullptr;
    }

    TaskMeta* waiter = head_;
    head_ = waiter->waiter.next.load(std::memory_order_relaxed);

    if (head_) {
        head_->waiter.prev.store(nullptr, std::memory_order_relaxed);
    } else {
        tail_ = nullptr;  // Queue is now empty
    }

    // Clear links and mark as not in queue
    waiter->waiter.next.store(nullptr, std::memory_order_relaxed);
    waiter->waiter.prev.store(nullptr, std::memory_order_relaxed);
    waiter->waiter.in_queue.store(false, std::memory_order_release);

    return waiter;
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

    TaskMeta* task = w->current_task();

    // 1. Check value first
    if (value_.load(std::memory_order_acquire) != expected_value) {
        return 0;
    }

    // 2. Initialize wait state in TaskMeta
    WaiterState& ws = task->waiter;
    ws.wakeup.store(false, std::memory_order_relaxed);
    ws.timed_out.store(false, std::memory_order_relaxed);
    ws.in_queue.store(false, std::memory_order_relaxed);
    ws.deadline_us = 0;
    ws.timer_id = 0;
    ws.next.store(nullptr, std::memory_order_relaxed);
    ws.prev.store(nullptr, std::memory_order_relaxed);

    // 3. Add to wait queue (prepend to head or append to tail)
    if (prepend) {
        AddToHead(task);
    } else {
        AddToTail(task);
    }

    // 4. Double-check value after adding to queue
    if (value_.load(std::memory_order_acquire) != expected_value) {
        // Value changed, remove ourselves from queue
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

    // 7. Set state to SUSPENDED
    task->state.store(TaskState::SUSPENDED, std::memory_order_release);

    // 8. Check wakeup flag AFTER setting SUSPENDED
    if (ws.wakeup.load(std::memory_order_acquire)) {
        // Wake already called
        task->state.store(TaskState::READY, std::memory_order_release);
        task->waiting_butex = nullptr;
        // Remove from queue if still there
        RemoveFromWaitQueue(task);
        // Cancel timer if any
        if (ws.timer_id != 0) {
            Scheduler::Instance().GetTimerThread()->Cancel(ws.timer_id);
        }
        return 0;
    }

    // 9. Suspend - we'll be resumed by Wake or timeout
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
    platform::FutexWake(&value_, count);

    int woken = 0;

    while (woken < count) {
        // Always pop from head (FIFO order for fairness)
        TaskMeta* waiter = PopFromHead();
        if (!waiter) break;

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

            // Check if task is SUSPENDED - only then re-queue it
            TaskState state = waiter->state.load(std::memory_order_acquire);
            if (state == TaskState::SUSPENDED) {
                waiter->state.store(TaskState::READY, std::memory_order_release);
                Scheduler::Instance().EnqueueTask(waiter);
            }
            // If not SUSPENDED, the task is still preparing to suspend
            // It will check wakeup and return without suspending
        }
    }
}