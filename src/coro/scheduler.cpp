// src/coro/scheduler.cpp
#include "coro/scheduler.h"
#include "coro/coroutine.h"
#include "bthread/core/scheduler.hpp"
#include <thread>
#include <chrono>
#include <cstdio>
#include <map>
#include <condition_variable>

namespace coro {

// === Timer system for sleep() ===
// Static variables for the sleep thread
static std::mutex sleep_mutex;
static std::condition_variable sleep_cv;
static std::multimap<std::chrono::steady_clock::time_point, CoroutineMeta*> sleep_queue_;
static std::atomic<bool> sleep_thread_running_{false};
static std::thread sleep_thread_;
static std::once_flag sleep_init_once_;

void StartSleepThread() {
    if (sleep_thread_running_.exchange(true)) return;

    sleep_thread_ = std::thread([] {
        while (sleep_thread_running_.load()) {
            std::unique_lock<std::mutex> lock(sleep_mutex);

            if (sleep_queue_.empty()) {
                sleep_cv.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }

            auto now = std::chrono::steady_clock::now();
            auto it = sleep_queue_.begin();

            // Wake up all coroutines whose wake time has passed
            while (it != sleep_queue_.end() && it->first <= now) {
                CoroutineMeta* meta = it->second;
                meta->state.store(bthread::TaskState::READY, std::memory_order_release);
                // Enqueue to unified scheduler
                bthread::Scheduler::Instance().Submit(meta);
                it = sleep_queue_.erase(it);
            }

            // Wait for next scheduled wake time or timeout
            if (!sleep_queue_.empty()) {
                auto next_time = sleep_queue_.begin()->first;
                auto wait_duration = next_time - std::chrono::steady_clock::now();
                if (wait_duration > std::chrono::milliseconds(0)) {
                    sleep_cv.wait_for(lock, wait_duration);
                }
            }
        }
    });
}

// SleepAwaiter::await_suspend implementation
bool SleepAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    CoroutineMeta* meta = current_coro_meta();
    if (!meta) {
        // Not in scheduler context, use blocking sleep
        std::this_thread::sleep_for(duration_);
        return false;
    }

    meta->state.store(bthread::TaskState::SUSPENDED, std::memory_order_release);

    // Calculate wake time
    auto wake_time = std::chrono::steady_clock::now() + duration_;

    {
        std::lock_guard<std::mutex> lock(sleep_mutex);
        sleep_queue_.emplace(wake_time, meta);
    }
    sleep_cv.notify_one();

    // Ensure sleep thread is running (call_once ensures single initialization)
    std::call_once(sleep_init_once_, StartSleepThread);

    return true;  // Suspend
}

// YieldAwaiter::await_suspend implementation
bool YieldAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    CoroutineMeta* meta = current_coro_meta();
    if (!meta) {
        // Not in scheduler context - should not happen
        return false;
    }

    // Re-queue the coroutine for later execution
    meta->state.store(bthread::TaskState::READY, std::memory_order_release);
    bthread::Scheduler::Instance().Submit(meta);

    return true;  // Suspend
}

// === CoroutineScheduler - now delegates to unified bthread::Scheduler ===

CoroutineScheduler& CoroutineScheduler::Instance() {
    static CoroutineScheduler instance;
    return instance;
}

CoroutineScheduler::~CoroutineScheduler() {
    Shutdown();
}

void CoroutineScheduler::Init() {
    // Initialize the unified scheduler instead
    bthread::Scheduler::Instance().Init();
}

void CoroutineScheduler::Shutdown() {
    // Shut down sleep thread if running
    if (sleep_thread_running_.load()) {
        sleep_thread_running_.store(false, std::memory_order_release);
        sleep_cv.notify_all();
        if (sleep_thread_.joinable()) {
            sleep_thread_.join();
        }
    }

    // Note: Unified scheduler shutdown is handled separately
}

bool CoroutineScheduler::running() const {
    return bthread::Scheduler::Instance().running();
}

bthread::TaskQueue& CoroutineScheduler::global_queue() {
    // This is deprecated - kept for backward compatibility
    static bthread::TaskQueue fallback_queue;
    return fallback_queue;
}

void CoroutineScheduler::EnqueueCoroutine(CoroutineMeta* meta) {
    // Delegate to unified scheduler
    bthread::Scheduler::Instance().Submit(meta);
}

CoroutineMeta* CoroutineScheduler::AllocMeta() {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    for (auto& meta : meta_pool_) {
        if (meta->state.load(std::memory_order_acquire) == bthread::TaskState::FINISHED ||
            meta->handle == nullptr) {
            // Reset meta
            meta->state.store(bthread::TaskState::READY, std::memory_order_release);
            meta->cancel_requested.store(false, std::memory_order_relaxed);
            meta->waiting_sync = nullptr;
            meta->next.store(nullptr, std::memory_order_relaxed);
            meta->owner_worker = nullptr;
            return meta.get();
        }
    }
    // Expand pool
    auto meta = std::make_unique<CoroutineMeta>();
    CoroutineMeta* ptr = meta.get();
    meta_pool_.push_back(std::move(meta));
    return ptr;
}

void CoroutineScheduler::FreeMeta(CoroutineMeta* meta) {
    // Acquire lock to synchronize with AllocMeta's handle check
    std::lock_guard<std::mutex> lock(meta_mutex_);
    meta->state.store(bthread::TaskState::FINISHED, std::memory_order_release);
    meta->handle = nullptr;
}

size_t CoroutineScheduler::worker_count() const {
    return bthread::Scheduler::Instance().worker_count();
}

} // namespace coro