// src/coro/scheduler.cpp
#include "coro/scheduler.h"
#include <thread>
#include <chrono>
#include <cstdio>

namespace coro {

thread_local CoroutineMeta* current_coro_meta_ = nullptr;

CoroutineScheduler& CoroutineScheduler::Instance() {
    static CoroutineScheduler instance;
    return instance;
}

CoroutineScheduler::~CoroutineScheduler() {
    Shutdown();
    // Notify all workers to wake up and exit
    queue_cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
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
    running_.store(false, std::memory_order_release);
    queue_cv_.notify_all();
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