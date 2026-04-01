// src/bthread/sync/event.cpp
#include "bthread/sync/event.hpp"
#include "bthread/scheduler.h"
#include "bthread/worker.h"
#include "bthread/platform/platform.h"
#include "coro/meta.h"

namespace bthread {

Event::Event(bool initial_set, bool auto_reset)
    : state_(initial_set), auto_reset_(auto_reset) {
}

Event::~Event() {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    // Clean up any remaining waiters
    while (waiter_head_) {
        WaiterNode* node = waiter_head_;
        waiter_head_ = node->next;
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
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    WaiterNode* node = new WaiterNode{task, nullptr};
    if (waiter_tail_) {
        waiter_tail_->next = node;
        waiter_tail_ = node;
    } else {
        waiter_head_ = waiter_tail_ = node;
    }
}

TaskMetaBase* Event::dequeue_waiter() {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    if (!waiter_head_) return nullptr;

    WaiterNode* node = waiter_head_;
    waiter_head_ = node->next;
    if (!waiter_head_) waiter_tail_ = nullptr;

    TaskMetaBase* task = node->task;
    delete node;
    return task;
}

void Event::wake_all_waiters() {
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

    meta->state.store(coro::CoroutineMeta::State::SUSPENDED, std::memory_order_release);
    meta->waiting_sync = &event_;
    event_.enqueue_waiter(meta);

    return true;
}

} // namespace bthread