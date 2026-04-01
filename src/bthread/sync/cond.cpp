// src/bthread/sync/cond.cpp
#include "bthread/sync/cond.hpp"
#include "bthread/sync/mutex.hpp"
#include "bthread/scheduler.h"
#include "bthread/worker.h"
#include "bthread/platform/platform.h"
#include "coro/meta.h"

#ifdef _WIN32
// Ensure Windows API functions are available
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <synchapi.h>
#else
#include <pthread.h>
#endif

namespace bthread {

CondVar::CondVar() {
#ifdef _WIN32
    native_cond_ = new CONDITION_VARIABLE();
    InitializeConditionVariable(static_cast<CONDITION_VARIABLE*>(native_cond_));
#else
    native_cond_ = new pthread_cond_t();
    pthread_cond_init(static_cast<pthread_cond_t*>(native_cond_), nullptr);
#endif
}

CondVar::~CondVar() {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    // Clean up any remaining waiters (should not happen in correct code)
    while (waiter_head_) {
        WaiterNode* node = waiter_head_;
        waiter_head_ = node->next;
        delete node;
    }

    if (native_cond_) {
#ifdef _WIN32
        delete static_cast<CONDITION_VARIABLE*>(native_cond_);
#else
        pthread_cond_destroy(static_cast<pthread_cond_t*>(native_cond_));
        delete static_cast<pthread_cond_t*>(native_cond_);
#endif
    }
}

void CondVar::wait(Mutex& mutex) {
    Worker* w = Worker::Current();

    if (w) {
        // Called from bthread - use our own waiting mechanism
        // Release mutex and wait
        mutex.unlock();

        // Get current task
        TaskMetaBase* task = w->current_task();
        if (task) {
            task->state.store(TaskState::SUSPENDED, std::memory_order_release);
            task->waiting_sync = this;
            enqueue_waiter(task);

            // Suspend
            w->SuspendCurrent();
        }

        // Re-acquire mutex
        mutex.lock();
    } else {
        // Called from pthread
        wait_pthread(mutex);
    }
}

void CondVar::wait_pthread(Mutex& mutex) {
#ifdef _WIN32
    // Use Windows native API
    PCONDITION_VARIABLE pcond = (PCONDITION_VARIABLE)native_cond_;
    PSRWLOCK plock = (PSRWLOCK)mutex.native_mutex_;
    // SleepConditionVariableSRW takes 4 parameters (Flags must be 0)
    SleepConditionVariableSRW(pcond, plock, INFINITE, 0);
#else
    pthread_cond_wait(
        static_cast<pthread_cond_t*>(native_cond_),
        static_cast<pthread_mutex_t*>(mutex.native_mutex_));
#endif
}

bool CondVar::wait_for(Mutex& mutex, std::chrono::milliseconds timeout) {
    Worker* w = Worker::Current();

    if (w) {
        // Called from bthread
        // For simplicity, use the same mechanism as wait()
        // TODO: Add timer-based wake-up
        wait(mutex);
        return true;
    } else {
        // Called from pthread
#ifdef _WIN32
        DWORD ms = static_cast<DWORD>(timeout.count());
        PCONDITION_VARIABLE pcond = (PCONDITION_VARIABLE)native_cond_;
        PSRWLOCK plock = (PSRWLOCK)mutex.native_mutex_;
        // SleepConditionVariableSRW takes 4 parameters (Flags must be 0)
        BOOL result = SleepConditionVariableSRW(pcond, plock, ms, 0);
        return result != FALSE;
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout.count() / 1000;
        ts.tv_nsec += (timeout.count() % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        return pthread_cond_timedwait(
            static_cast<pthread_cond_t*>(native_cond_),
            static_cast<pthread_mutex_t*>(mutex.native_mutex_),
            &ts) == 0;
#endif
    }
}

void CondVar::notify_one() {
    TaskMetaBase* waiter = dequeue_waiter();
    if (waiter) {
        waiter->state.store(TaskState::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        Scheduler::Instance().Submit(waiter);
    } else {
        // Wake pthread waiters
#ifdef _WIN32
        WakeConditionVariable(static_cast<CONDITION_VARIABLE*>(native_cond_));
#else
        pthread_cond_signal(static_cast<pthread_cond_t*>(native_cond_));
#endif
    }
}

void CondVar::notify_all() {
    // Wake all coroutine/bthread waiters
    std::vector<TaskMetaBase*> waiters;
    {
        std::lock_guard<std::mutex> lock(waiters_mutex_);
        while (waiter_head_) {
            WaiterNode* node = waiter_head_;
            waiter_head_ = node->next;
            waiters.push_back(node->task);
            delete node;
        }
        waiter_tail_ = nullptr;
    }

    for (TaskMetaBase* waiter : waiters) {
        waiter->state.store(TaskState::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        Scheduler::Instance().Submit(waiter);
    }

    // Wake pthread waiters
#ifdef _WIN32
    WakeAllConditionVariable(static_cast<CONDITION_VARIABLE*>(native_cond_));
#else
    pthread_cond_broadcast(static_cast<pthread_cond_t*>(native_cond_));
#endif
}

void CondVar::enqueue_waiter(TaskMetaBase* task) {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    WaiterNode* node = new WaiterNode{task, nullptr};
    if (waiter_tail_) {
        waiter_tail_->next = node;
        waiter_tail_ = node;
    } else {
        waiter_head_ = waiter_tail_ = node;
    }
}

TaskMetaBase* CondVar::dequeue_waiter() {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    if (!waiter_head_) return nullptr;

    WaiterNode* node = waiter_head_;
    waiter_head_ = node->next;
    if (!waiter_head_) waiter_tail_ = nullptr;

    TaskMetaBase* task = node->task;
    delete node;
    return task;
}

// ========== WaitAwaiter Implementation ==========

bool CondVar::WaitAwaiter::await_suspend(std::coroutine_handle<> h) {
    coro::CoroutineMeta* meta = coro::current_coro_meta();
    if (!meta) {
        throw std::logic_error("Cannot co_await cond.wait_async() outside of scheduler context");
    }

    // Release mutex
    mutex_.unlock();

    // Add to waiters
    meta->state.store(bthread::TaskState::SUSPENDED, std::memory_order_release);
    meta->waiting_sync = &cond_;
    cond_.enqueue_waiter(meta);

    return true;
}

void CondVar::WaitAwaiter::await_resume() {
    // Re-acquire mutex
    while (!mutex_.try_lock()) {
        std::this_thread::yield();
    }
}

} // namespace bthread