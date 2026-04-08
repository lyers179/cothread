// src/bthread/sync/mutex.cpp
#include "bthread/sync/mutex.hpp"
#include "bthread/core/scheduler.hpp"
#include "bthread/core/worker.hpp"
#include "bthread/sync/butex.hpp"
#include "bthread/platform/platform.h"
#include "coro/meta.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace bthread {

// ========== Mutex Implementation ==========

Mutex::Mutex() {
#ifdef _WIN32
    native_mutex_ = new SRWLOCK();
    InitializeSRWLock(static_cast<SRWLOCK*>(native_mutex_));
#else
    native_mutex_ = new pthread_mutex_t();
    pthread_mutex_init(static_cast<pthread_mutex_t*>(native_mutex_), nullptr);
#endif
}

Mutex::~Mutex() {
    // Assert no waiters
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    if (waiter_head_ != nullptr) {
        // Leak waiters rather than crash - should not happen in correct code
    }

    if (butex_) {
        delete static_cast<Butex*>(butex_.load(std::memory_order_relaxed));
    }

    if (native_mutex_) {
#ifdef _WIN32
        delete static_cast<SRWLOCK*>(native_mutex_);
#else
        pthread_mutex_destroy(static_cast<pthread_mutex_t*>(native_mutex_));
        delete static_cast<pthread_mutex_t*>(native_mutex_);
#endif
    }
}

void Mutex::lock() {
    Worker* w = Worker::Current();

    if (w) {
        // Called from bthread
        lock_bthread();
    } else {
        // Called from pthread
        lock_pthread();
    }
}

void Mutex::lock_bthread() {
    // Fast path: try to acquire without waiters flag
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, LOCKED,
            std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
    }

    // Create butex if needed
    if (!butex_.load(std::memory_order_acquire)) {
        Butex* new_butex = new Butex();
        void* expected = nullptr;
        if (butex_.compare_exchange_strong(expected, new_butex,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Successfully set
        } else {
            delete new_butex;
        }
    }

    // Key fairness optimization from official bthread:
    // First wait: use FIFO (append to tail) for fairness
    // After being woken but losing the race: use LIFO (prepend to head)
    bool first_wait = true;

    while (true) {
        expected = state_.load(std::memory_order_acquire);

        if ((expected & LOCKED) == 0) {
            // Lock appears free, try to acquire
            uint32_t new_val = LOCKED | (expected & HAS_WAITERS);
            if (state_.compare_exchange_strong(expected, new_val,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            continue;
        }

        // Lock is held, mark that we're waiting
        if ((expected & HAS_WAITERS) == 0) {
            if (!state_.compare_exchange_strong(expected, expected | HAS_WAITERS,
                    std::memory_order_release, std::memory_order_relaxed)) {
                continue;
            }
        }

        // Wait on butex
        Butex* butex = static_cast<Butex*>(butex_.load(std::memory_order_acquire));

        // Double-check lock state before getting generation
        expected = state_.load(std::memory_order_acquire);
        if ((expected & LOCKED) == 0) {
            uint32_t new_val = LOCKED | (expected & HAS_WAITERS);
            if (state_.compare_exchange_strong(expected, new_val,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            // CAS failed, re-read state for the loop
            continue;
        }

        int generation = butex->value();

        // Wait (first wait: FIFO, subsequent: LIFO)
        int result = butex->Wait(generation, nullptr, !first_wait);
        if (result == ECANCELED) {
            // Scheduler is shutting down - break out of loop
            return;
        }
        first_wait = false;
    }
}

void Mutex::lock_pthread() {
    // Acquire both native mutex and state_ for consistency
    // This prevents bthread from acquiring while pthread holds the lock
#ifdef _WIN32
    AcquireSRWLockExclusive(static_cast<SRWLOCK*>(native_mutex_));
#else
    pthread_mutex_lock(static_cast<pthread_mutex_t*>(native_mutex_));
#endif
    // Mark state as LOCKED so bthreads will wait
    state_.fetch_or(LOCKED, std::memory_order_release);
}

bool Mutex::try_lock() {
    Worker* w = Worker::Current();

    if (w) {
        // Called from bthread - use atomic state
        uint32_t expected = 0;
        if (state_.compare_exchange_strong(expected, LOCKED,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
        return false;
    } else {
        // Called from pthread - use native mutex
#ifdef _WIN32
        return TryAcquireSRWLockExclusive(static_cast<SRWLOCK*>(native_mutex_));
#else
        return pthread_mutex_trylock(static_cast<pthread_mutex_t*>(native_mutex_)) == 0;
#endif
    }
}

void Mutex::unlock() {
    Worker* w = Worker::Current();

    if (!w) {
        // Called from pthread - use native mutex
        unlock_pthread();
        return;
    }

    // Called from bthread
    // Check for coroutine waiters first
    TaskMetaBase* waiter = dequeue_waiter();

    if (waiter) {
        // Transfer lock to coroutine waiter
        waiter->state.store(TaskState::READY, std::memory_order_release);
        waiter->waiting_sync = nullptr;
        Scheduler::Instance().Submit(waiter);
        // LOCKED stays set - waiter now owns it
        return;
    }

    // Check if there are bthread waiters
    uint32_t old_state = state_.fetch_and(~LOCKED, std::memory_order_release);

    if (old_state & HAS_WAITERS) {
        // Wake one bthread waiter via butex
        Butex* butex = static_cast<Butex*>(butex_.load(std::memory_order_acquire));
        if (butex) {
            butex->set_value(butex->value() + 1);
            butex->Wake(1);
        }
    }
}

void Mutex::unlock_pthread() {
    // Clear LOCKED state and wake any bthread waiters
    uint32_t old_state = state_.fetch_and(~LOCKED, std::memory_order_release);

    if (old_state & HAS_WAITERS) {
        Butex* butex = static_cast<Butex*>(butex_.load(std::memory_order_acquire));
        if (butex) {
            butex->set_value(butex->value() + 1);
            butex->Wake(1);
        }
    }

#ifdef _WIN32
    ReleaseSRWLockExclusive(static_cast<SRWLOCK*>(native_mutex_));
#else
    pthread_mutex_unlock(static_cast<pthread_mutex_t*>(native_mutex_));
#endif
}

void Mutex::enqueue_waiter(TaskMetaBase* task) {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    WaiterNode* node = new WaiterNode{task, nullptr};
    if (waiter_tail_) {
        waiter_tail_->next = node;
        waiter_tail_ = node;
    } else {
        waiter_head_ = waiter_tail_ = node;
    }
}

TaskMetaBase* Mutex::dequeue_waiter() {
    std::lock_guard<std::mutex> lock(waiters_mutex_);
    if (!waiter_head_) return nullptr;

    WaiterNode* node = waiter_head_;
    waiter_head_ = node->next;
    if (!waiter_head_) waiter_tail_ = nullptr;

    TaskMetaBase* task = node->task;
    delete node;
    return task;
}

// ========== LockAwaiter Implementation ==========

bool Mutex::LockAwaiter::await_suspend(std::coroutine_handle<> h) {
    // Get CoroutineMeta
    coro::CoroutineMeta* meta = coro::current_coro_meta();
    if (!meta) {
        throw std::logic_error("Cannot co_await mutex.lock_async() outside of scheduler context");
    }

    // Try to acquire lock one more time
    if (mutex_.try_lock()) {
        return false;
    }

    // Mark that we have waiters
    mutex_.state_.fetch_or(HAS_WAITERS, std::memory_order_release);

    // Retry after setting HAS_WAITERS
    if (mutex_.try_lock()) {
        mutex_.state_.fetch_and(~HAS_WAITERS, std::memory_order_release);
        return false;
    }

    // Suspend coroutine
    meta->state.store(bthread::TaskState::SUSPENDED, std::memory_order_release);
    meta->waiting_sync = &mutex_;

    mutex_.enqueue_waiter(meta);

    return true;
}

} // namespace bthread