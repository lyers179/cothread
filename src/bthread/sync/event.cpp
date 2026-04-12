// src/bthread/sync/event.cpp
#include "bthread/sync/event.hpp"
#include "bthread/core/scheduler.hpp"
#include "bthread/core/worker.hpp"
#include "bthread/platform/platform.h"
#include "coro/meta.h"

namespace bthread {

Event::Event(bool initial_set, bool auto_reset)
    : state_(initial_set), auto_reset_(auto_reset) {
}

Event::~Event() {
    // Drain remaining waiters from lock-free queue
    while (EventWaiterNode* node = waiter_queue_.Pop()) {
        delete node;
    }
}

void Event::wait() {
    // Fast path: already set
    if (state_.load(std::memory_order_acquire)) {
        if (auto_reset_) {
            state_.store(false, std::memory_order_release);
        }
        return;
    }

    Worker* w = Worker::Current();
    if (w) {
        // Called from bthread
        TaskMetaBase* task = w->current_task();
        if (task) {
            task->state.store(TaskState::SUSPENDED, std::memory_order_release);
            task->waiting_sync = this;
            enqueue_waiter(task);
            w->SuspendCurrent();
        }
    } else {
        // Called from pthread - use spin-yield
        while (!state_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (auto_reset_) {
            state_.store(false, std::memory_order_release);
        }
    }
}

bool Event::wait_for(std::chrono::milliseconds timeout) {
    // Fast path: already set
    if (state_.load(std::memory_order_acquire)) {
        if (auto_reset_) {
            state_.store(false, std::memory_order_release);
        }
        return true;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;

    Worker* w = Worker::Current();
    if (w) {
        // Called from bthread
        // TODO: Use timer for proper timeout
        TaskMetaBase* task = w->current_task();
        if (task) {
            task->state.store(TaskState::SUSPENDED, std::memory_order_release);
            task->waiting_sync = this;
            enqueue_waiter(task);
            w->SuspendCurrent();
            return true;  // For now, assume no timeout
        }
    } else {
        // Called from pthread
        while (!state_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;  // Timeout
            }
            std::this_thread::yield();
        }
        if (auto_reset_) {
            state_.store(false, std::memory_order_release);
        }
        return true;
    }

    return false;
}

void Event::set() {
    state_.store(true, std::memory_order_release);
    wake_all_waiters();
}

void Event::reset() {
    state_.store(false, std::memory_order_release);
}

void Event::enqueue_waiter(TaskMetaBase* task) {
    EventWaiterNode* node = new EventWaiterNode{task};
    waiter_queue_.Push(node);  // Lock-free MPSC push
}

TaskMetaBase* Event::dequeue_waiter() {
    EventWaiterNode* node = waiter_queue_.Pop();  // Lock-free pop
    if (!node) return nullptr;
    TaskMetaBase* task = node->task;
    delete node;
    return task;
}

void Event::wake_all_waiters() {
    // Drain entire queue - lock-free
    std::vector<TaskMetaBase*> waiters;
    while (EventWaiterNode* node = waiter_queue_.Pop()) {
        waiters.push_back(node->task);
        delete node;
    }

    for (TaskMetaBase* waiter : waiters) {
        waiter->state.store(TaskState::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        Scheduler::Instance().Submit(waiter);
    }
}

// ========== WaitAwaiter Implementation ==========

bool Event::WaitAwaiter::await_suspend(std::coroutine_handle<> h) {
    coro::CoroutineMeta* meta = coro::current_coro_meta();
    if (!meta) {
        throw std::logic_error("Cannot co_await event.wait_async() outside of scheduler context");
    }

    // Double-check state
    if (event_.is_set()) {
        return false;
    }

    meta->state.store(bthread::TaskState::SUSPENDED, std::memory_order_release);
    meta->waiting_sync = &event_;
    event_.enqueue_waiter(meta);

    return true;
}

} // namespace bthread