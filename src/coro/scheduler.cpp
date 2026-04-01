// src/coro/scheduler.cpp
#include "coro/scheduler.h"
#include "coro/coroutine.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <map>
#include <condition_variable>

namespace coro {

thread_local CoroutineMeta* current_coro_meta_ = nullptr;

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
                meta->state.store(CoroutineMeta::READY, std::memory_order_release);
                CoroutineScheduler::Instance().EnqueueCoroutine(meta);
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

    meta->state.store(CoroutineMeta::SUSPENDED, std::memory_order_release);

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

CoroutineScheduler& CoroutineScheduler::Instance() {
    static CoroutineScheduler instance;
    return instance;
}

CoroutineScheduler::~CoroutineScheduler() {
    Shutdown();
}

void CoroutineScheduler::Init() {
    std::call_once(init_once_, [this] {
        InitMetaPool(256);
        running_.store(true, std::memory_order_release);
        StartCoroutineWorkers(4);  // 4 coroutine workers by default
        initialized_.store(true, std::memory_order_release);
    });
}

void CoroutineScheduler::Shutdown() {
    // Guard against multiple shutdown calls
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // Signal all workers to stop
    running_.store(false, std::memory_order_release);
    queue_cv_.notify_all();

    // Shut down sleep thread if running
    if (sleep_thread_running_.load()) {
        sleep_thread_running_.store(false, std::memory_order_release);
        sleep_cv.notify_all();
        if (sleep_thread_.joinable()) {
            sleep_thread_.join();
        }
    }

    // Join all worker threads
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workers_.clear();
}

void CoroutineScheduler::InitMetaPool(size_t count) {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    for (size_t i = 0; i < count; ++i) {
        meta_pool_.push_back(std::make_unique<CoroutineMeta>());
    }
}

CoroutineMeta* CoroutineScheduler::AllocMeta() {
    std::lock_guard<std::mutex> lock(meta_mutex_);
    for (auto& meta : meta_pool_) {
        if (meta->state.load(std::memory_order_acquire) == CoroutineMeta::FINISHED ||
            meta->handle == nullptr) {
            // Reset meta
            meta->state.store(CoroutineMeta::READY, std::memory_order_release);
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
    meta->state.store(CoroutineMeta::FINISHED, std::memory_order_release);
    meta->handle = nullptr;
}

void CoroutineScheduler::EnqueueCoroutine(CoroutineMeta* meta) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        global_queue_.Push(meta);
    }
    queue_cv_.notify_one();
}

void CoroutineScheduler::StartCoroutineWorkers(int count) {
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back([this] {
            CoroutineWorkerLoop();
        });
    }
}

void CoroutineScheduler::CoroutineWorkerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        CoroutineMeta* meta = nullptr;

        // Pop must be synchronized since CoroutineQueue is MPSC, not MPMC
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            meta = global_queue_.Pop();
        }

        if (!meta) {
            // No work, wait for notification or timeout
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !running_.load(std::memory_order_acquire) || !global_queue_.Empty();
            });
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        // Null handle check - skip if handle is invalid
        if (!meta->handle) {
            FreeMeta(meta);
            continue;
        }

        // Set current coroutine (for yield operations)
        current_coro_meta_ = meta;
        meta->state.store(CoroutineMeta::RUNNING, std::memory_order_release);

        // Resume coroutine with exception handling
        try {
            meta->handle.resume();
        } catch (const std::exception& e) {
            // Log exception and mark coroutine as finished
            // In production, this should integrate with a logging system
            std::fprintf(stderr, "Coroutine exception: %s\n", e.what());
            FreeMeta(meta);
            current_coro_meta_ = nullptr;
            continue;
        } catch (...) {
            // Catch any unknown exceptions to prevent worker thread crash
            std::fprintf(stderr, "Coroutine exception: unknown exception\n");
            FreeMeta(meta);
            current_coro_meta_ = nullptr;
            continue;
        }

        // Clear current coroutine
        current_coro_meta_ = nullptr;

        // Handle post-resume state - check if coroutine is done
        if (meta->handle.done()) {
            FreeMeta(meta);
        }
    }
}

} // namespace coro