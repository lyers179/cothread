#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "bthread/task_meta.h"
#include "bthread/worker.h"
#include "bthread/global_queue.h"

namespace bthread {

// Forward declarations
class TimerThread;
class Butex;

// Global scheduler managing all workers
class Scheduler {
public:
    // Get singleton instance
    static Scheduler& Instance();

    // Initialize scheduler (lazy)
    void Init();

    // Shutdown scheduler
    void Shutdown();

    // Check if running
    bool running() const {
        return running_.load(std::memory_order_acquire);
    }

    // Enqueue task for execution
    void EnqueueTask(TaskMeta* task);

    // Get worker count
    int32_t worker_count() const {
        return worker_count_.load(std::memory_order_acquire);
    }

    // Get worker by index
    Worker* GetWorker(int index);

    // Get global queue
    GlobalQueue& global_queue() { return global_queue_; }
    const GlobalQueue& global_queue() const { return global_queue_; }

    // Get task group
    TaskGroup& task_group() { return task_group_; }

    // Set worker count (must be called before Init)
    void set_worker_count(int32_t count) {
        configured_count_ = count;
    }

    // Get timer thread (lazy init)
    TimerThread* GetTimerThread();

    // Wake butex waiters
    void WakeButex(void* butex, int count);

    // Wake idle workers
    void WakeIdleWorkers(int count);

private:
    Scheduler();
    ~Scheduler();

    // Disable copy and move
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Start worker threads
    void StartWorkers(int count);

    // Wake all workers
    void WakeAllWorkers();

    std::vector<Worker*> workers_;
    std::mutex workers_mutex_;
    std::atomic<int32_t> worker_count_{0};
    int32_t configured_count_{0};

    GlobalQueue global_queue_;
    TaskGroup& task_group_;

    std::unique_ptr<TimerThread> timer_thread_;
    std::once_flag timer_init_flag_;
    std::mutex timer_mutex_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::once_flag init_once_;
};

} // namespace bthread