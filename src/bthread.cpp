#include "bthread.h"
#include "bthread/task_meta.h"
#include "bthread/task_group.h"
#include "bthread/worker.h"
#include "bthread/scheduler.h"
#include "bthread/butex.h"
#include "bthread/mutex.h"
#include "bthread/cond.h"
#include "bthread/once.h"
#include "bthread/timer_thread.h"
#include "bthread/platform/platform.h"

#include <cstring>
#include <cerrno>
#include <ctime>
#include <thread>

using namespace bthread;

// ========== bthread_t helpers ==========
namespace {

constexpr size_t NS_PER_US = 1000;
constexpr size_t US_PER_MS = 1000;
constexpr size_t MS_PER_SEC = 1000;
constexpr size_t US_PER_SEC = 1000000;

struct timespec ToAbsoluteTimeout(uint64_t delay_us) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    uint64_t delay_sec = delay_us / US_PER_SEC;
    uint64_t delay_ns = (delay_us % US_PER_SEC) * NS_PER_US;

    ts.tv_sec += delay_sec;
    ts.tv_nsec += delay_ns;

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

    // Set up stack
    size_t stack_size = attr ? attr->stack_size : BTHREAD_STACK_SIZE_DEFAULT;
    if (!task->stack) {
        task->stack = platform::AllocateStack(stack_size);
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
    platform::MakeContext(&task->context, task->stack, task->stack_size,
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
    if (w && w->current_task() == task) {
        return EDEADLK;
    }

    // Check if already finished
    if (task->state.load(std::memory_order_acquire) == TaskState::FINISHED) {
        if (retval) *retval = task->result;
        if (task->Release()) {
            GetTaskGroup().DeallocTaskMeta(task);
        }
        return 0;
    }

    // Wait for completion
    task->join_waiters.fetch_add(1, std::memory_order_acq_rel);
    static_cast<Butex*>(task->join_butex)->Wait(0, nullptr);
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
    if (!w || !w->current_task()) {
        return 0;  // Not in a bthread
    }
    TaskMeta* task = w->current_task();
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
    if (!w || !w->current_task()) {
        // Called from pthread - just return
        return;
    }

    TaskMeta* task = w->current_task();
    task->result = retval;
    task->state.store(TaskState::FINISHED, std::memory_order_release);

    // Decrement ref count
    if (task->Release()) {
        GetTaskGroup().DeallocTaskMeta(task);
    }

    // Switch back to scheduler (never returns)
    w->SuspendCurrent();
}

// ========== Synchronization Primitives ==========

// Mutex implementation will be in mutex.cpp
// Condition variable implementation will be in cond.cpp
// Once implementation will be in once.cpp

// ========== Timer ==========

bthread_timer_t bthread_timer_add(void (*callback)(void*), void* arg,
                                   const struct timespec* delay) {
    if (!callback || !delay) return -1;

    Scheduler::Instance().Init();

    uint64_t delay_us = static_cast<uint64_t>(delay->tv_sec) * US_PER_SEC +
                       delay->tv_nsec / NS_PER_US;

    struct timespec ts = ToAbsoluteTimeout(delay_us);
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