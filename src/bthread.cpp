#include "bthread.h"
#include "bthread/core/task_meta.hpp"
#include "bthread/core/task_group.hpp"
#include "bthread/core/worker.hpp"
#include "bthread/core/scheduler.hpp"
#include "bthread/sync/butex.hpp"
#include "bthread/api/once.hpp"
#include "bthread/detail/timer_thread.hpp"
#include "bthread/platform/platform.h"

#include <cstring>
#include <thread>

using namespace bthread;
using namespace bthread::platform;

// ========== bthread_t helpers ==========
namespace {

constexpr size_t NS_PER_US = 1000;
constexpr size_t US_PER_MS = 1000;
constexpr size_t MS_PER_SEC = 1000;
constexpr size_t US_PER_SEC = 1000000;

platform::timespec ToAbsoluteTimeout(uint64_t delay_us) {
    platform::timespec ts;
    int64_t now_us = GetTimeOfDayUs();

    uint64_t delay_sec = delay_us / US_PER_SEC;
    uint64_t delay_ns = (delay_us % US_PER_SEC) * NS_PER_US;

    ts.tv_sec = static_cast<int64_t>(now_us / US_PER_SEC) + static_cast<int64_t>(delay_sec);
    ts.tv_nsec = static_cast<int64_t>(now_us % US_PER_SEC * NS_PER_US) + static_cast<int64_t>(delay_ns);

    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    return ts;
}

} // namespace

// ========== bthread API ==========

int bthread_create(bthread_t* tid, const bthread_attr_t* attr,
                   void* (*fn)(void*), void* arg) {
    if (!tid || !fn) return EINVAL;

    Scheduler::Instance().Init();

    // Allocate TaskMeta
    TaskMeta* task = GetTaskGroup().AllocTaskMeta();
    if (!task) return EAGAIN;

    // Set up stack - use worker's pool if available
    size_t stack_size = attr ? attr->stack_size : BTHREAD_STACK_SIZE_DEFAULT;
    if (!task->stack) {
        Worker* w = Worker::Current();
        if (w) {
            task->stack = w->AcquireStack(stack_size);
        } else {
            task->stack = AllocateStack(stack_size);
        }
        if (!task->stack) {
            GetTaskGroup().DeallocTaskMeta(task);
            return ENOMEM;
        }
        task->stack_size = stack_size;
    }

    // Initialize
    task->fn = fn;
    task->arg = arg;
    task->result = nullptr;
    task->state.store(TaskState::READY, std::memory_order_relaxed);
    task->ref_count.store(2, std::memory_order_relaxed);  // Creator + joinable
    task->join_waiters.store(0, std::memory_order_relaxed);
    task->join_butex = new Butex();

    // Set up context
    MakeContext(&task->context, task->stack, task->stack_size,
                detail::BthreadEntry, task);

    // Encode bthread_t
    *tid = GetTaskGroup().EncodeId(task->slot_index, task->generation);

    // Enqueue
    Scheduler::Instance().EnqueueTask(task);

    return 0;
}

int bthread_join(bthread_t tid, void** retval) {
    TaskMeta* task = GetTaskGroup().DecodeId(tid);
    if (!task) return ESRCH;

    // Check if trying to join self
    Worker* w = Worker::Current();
    if (w) {
        TaskMetaBase* current = w->current_task();
        if (current && current->type == TaskType::BTHREAD &&
            static_cast<TaskMeta*>(current) == task) {
            return EDEADLK;
        }
    }

    // Capture generation BEFORE checking state to avoid race condition
    Butex* join_butex = static_cast<Butex*>(task->join_butex);
    int generation = join_butex->value();

    // Check if already finished
    if (task->state.load(std::memory_order_acquire) == TaskState::FINISHED) {
        if (retval) *retval = task->result;
        if (task->Release()) {
            GetTaskGroup().DeallocTaskMeta(task);
        }
        return 0;
    }

    // Wait for completion using generation mechanism
    task->join_waiters.fetch_add(1, std::memory_order_acq_rel);
    join_butex->Wait(generation, nullptr);
    task->join_waiters.fetch_sub(1, std::memory_order_acq_rel);

    if (retval) *retval = task->result;
    if (task->Release()) {
        GetTaskGroup().DeallocTaskMeta(task);
    }

    return 0;
}

int bthread_detach(bthread_t tid) {
    TaskMeta* task = GetTaskGroup().DecodeId(tid);
    if (!task) return ESRCH;

    // Decrement ref count (was 2 for joinable, now 1)
    if (task->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Already finished, recycle immediately
        GetTaskGroup().DeallocTaskMeta(task);
    }

    return 0;
}

bthread_t bthread_self(void) {
    Worker* w = Worker::Current();
    if (!w) return 0;

    TaskMetaBase* base_task = w->current_task();
    if (!base_task || base_task->type != TaskType::BTHREAD) {
        return 0;  // Not in a bthread
    }

    TaskMeta* task = static_cast<TaskMeta*>(base_task);
    return GetTaskGroup().EncodeId(task->slot_index, task->generation);
}

int bthread_yield(void) {
    Worker* w = Worker::Current();
    if (w == nullptr) {
        // Called from pthread
        std::this_thread::yield();
        return 0;
    }
    // Called from bthread
    return w->YieldCurrent();
}

void bthread_exit(void* retval) {
    Worker* w = Worker::Current();
    if (!w) return;

    TaskMetaBase* base_task = w->current_task();
    if (!base_task || base_task->type != TaskType::BTHREAD) {
        return;
    }

    TaskMeta* task = static_cast<TaskMeta*>(base_task);
    task->result = retval;
    task->state.store(TaskState::FINISHED, std::memory_order_release);

    // Switch back to scheduler (never returns)
    w->SuspendCurrent();
}

// ========== Timer ==========

bthread_timer_t bthread_timer_add(void (*callback)(void*), void* arg,
                                   const bthread_timespec* delay) {
    if (!callback || !delay) return -1;

    Scheduler::Instance().Init();

    uint64_t delay_us = static_cast<uint64_t>(delay->tv_sec) * US_PER_SEC +
                       delay->tv_nsec / NS_PER_US;

    platform::timespec ts = ToAbsoluteTimeout(delay_us);
    return Scheduler::Instance().GetTimerThread()->Schedule(callback, arg, &ts);
}

int bthread_timer_cancel(bthread_timer_t timer_id) {
    if (timer_id < 0) return EINVAL;

    bool cancelled = Scheduler::Instance().GetTimerThread()->Cancel(timer_id);
    return cancelled ? 0 : ESRCH;
}

// ========== Global Configuration ==========

int bthread_set_worker_count(int count) {
    if (count <= 0) return EINVAL;

    Scheduler& sched = Scheduler::Instance();
    if (sched.worker_count() > 0) {
        return EBUSY;  // Already initialized
    }

    sched.set_worker_count(count);
    return 0;
}

int bthread_get_worker_count(void) {
    return Scheduler::Instance().worker_count();
}

void bthread_shutdown(void) {
    Scheduler::Instance().Shutdown();
}