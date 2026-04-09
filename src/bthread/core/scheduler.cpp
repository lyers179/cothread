#include "bthread/core/scheduler.hpp"
#include "bthread/core/task_group.hpp"
#include "bthread/detail/timer_thread.hpp"
#include "bthread/sync/butex.hpp"
#include "bthread/platform/platform.h"
#include "coro/meta.h"
#include "coro/coroutine.h"

#include <thread>
#include <chrono>

namespace bthread {

Scheduler::Scheduler() {}

Scheduler::~Scheduler() {
    Shutdown();
}

Scheduler& Scheduler::Instance() {
    static Scheduler instance;
    return instance;
}

void Scheduler::Init() {
    std::call_once(init_once_, [this] {
        // Set up stack overflow handler
        platform::SetupStackOverflowHandler();

        int n = configured_count_;
        if (n <= 0) {
            n = std::thread::hardware_concurrency();
            if (n == 0) n = 4;
        }

        // Set running flag BEFORE starting workers to prevent race condition
        running_.store(true, std::memory_order_release);

        StartWorkers(n);
        GetTaskGroup().set_worker_count(n);
        initialized_.store(true, std::memory_order_release);
    });
}

void Scheduler::Init(int worker_count) {
    configured_count_ = worker_count;
    Init();
}

void Scheduler::Shutdown() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);

    // Get worker count before clearing
    int num_workers = worker_count_.load(std::memory_order_acquire);

    // Stop all workers multiple times with delays
    // This ensures the stop flag is set and workers are woken
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto* w : workers_) {
            w->Stop();
        }

        // Wake all SUSPENDED tasks so they can detect shutdown and complete
        auto suspended_tasks = GetTaskGroup().GetSuspendedTasks();
        for (auto* task : suspended_tasks) {
            // Clear is_waiting flag
            task->is_waiting.store(false, std::memory_order_release);
            task->waiting_butex = nullptr;
            // Set state to READY and enqueue
            task->state.store(TaskState::READY, std::memory_order_release);
            EnqueueTask(task);
        }

        // Shorter sleep, more attempts
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Give workers a bit more time to exit their loops
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Join all workers
    std::vector<Worker*> workers_copy;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_copy = workers_;
    }

    for (auto* w : workers_copy) {
        platform::JoinThread(w->thread());
        delete w;
    }

    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.clear();
    }
    worker_count_.store(0, std::memory_order_release);

    if (timer_thread_) {
        timer_thread_->Stop();
        timer_thread_.reset();
    }
}

void Scheduler::StartWorkers(int count) {
    std::lock_guard<std::mutex> lock(workers_mutex_);

    workers_.reserve(count);
    for (int i = 0; i < count; ++i) {
        Worker* w = new Worker(i);
        w->set_thread(platform::CreateThread([](void* arg) {
            static_cast<Worker*>(arg)->Run();
        }, w));
        workers_.push_back(w);
        // Also store in atomic array for lock-free wake
        workers_atomic_[i].store(w, std::memory_order_release);
    }
    worker_count_.store(count, std::memory_order_release);
}

void Scheduler::WakeAllWorkers() {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto* w : workers_) {
        w->WakeUp();
    }
}

Worker* Scheduler::GetWorker(int index) {
    // Use atomic array for lock-free access
    if (index >= 0 && index < MAX_WORKERS) {
        return workers_atomic_[index].load(std::memory_order_acquire);
    }
    return nullptr;
}

TaskGroup& Scheduler::task_group() {
    return GetTaskGroup();
}

// ========== Unified Task Submission ==========

void Scheduler::Submit(TaskMetaBase* task) {
    // Auto-initialize if not already initialized
    if (!initialized_.load(std::memory_order_acquire)) {
        Init();
    }

    // Set state to READY
    task->state.store(TaskState::READY, std::memory_order_release);

    // First try to push to current worker's local queue
    Worker* current = Worker::Current();
    if (current) {
        current->local_queue().Push(task);
        // Wake up idle workers if we just pushed to a worker that might be sleeping
        // This handles the case where the current worker is about to suspend/go idle
        WakeIdleWorkers(1);
    } else {
        // Not in a worker thread, push to global queue
        global_queue_.Push(task);
        // Wake up ONE idle worker - no need to wake all workers for a single task
        // Workers will steal from global queue if their local queue is empty
        WakeIdleWorkers(1);
    }
}

void Scheduler::EnqueueTask(TaskMeta* task) {
    // Legacy bthread method - forwards to Submit
    Submit(static_cast<TaskMetaBase*>(task));
}

// ========== Coroutine Support ==========

template<typename T>
coro::Task<T> Scheduler::Spawn(coro::Task<T> task) {
    // Auto-initialize if not already initialized
    if (!initialized_.load(std::memory_order_acquire)) {
        Init();
    }

    // Allocate CoroutineMeta
    coro::CoroutineMeta* meta = new coro::CoroutineMeta();
    meta->handle = task.handle();
    meta->state.store(TaskState::READY, std::memory_order_release);

    // Store CoroutineMeta in promise
    task.handle().promise().set_meta(meta);

    Submit(meta);
    return std::move(task);
}

template<typename T>
coro::SafeTask<T> Scheduler::Spawn(coro::SafeTask<T> task) {
    // Auto-initialize if not already initialized
    if (!initialized_.load(std::memory_order_acquire)) {
        Init();
    }

    // Similar to Task<T> spawning
    coro::CoroutineMeta* meta = new coro::CoroutineMeta();
    meta->handle = task.handle();
    meta->state.store(TaskState::READY, std::memory_order_release);

    task.handle().promise().set_meta(meta);

    Submit(meta);
    return std::move(task);
}

// Explicit template instantiation
template coro::Task<void> Scheduler::Spawn(coro::Task<void>);
template coro::Task<int> Scheduler::Spawn(coro::Task<int>);
template coro::SafeTask<void> Scheduler::Spawn(coro::SafeTask<void>);
template coro::SafeTask<int> Scheduler::Spawn(coro::SafeTask<int>);

TimerThread* Scheduler::GetTimerThread() {
    std::call_once(timer_init_flag_, [this] {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (!timer_thread_) {
            timer_thread_ = std::make_unique<TimerThread>();
            timer_thread_->Start();
        }
    });
    return timer_thread_.get();
}

void Scheduler::WakeButex(void* butex, int count) {
    // Forward to Butex implementation
    if (butex) {
        static_cast<Butex*>(butex)->Wake(count);
    }
}

void Scheduler::WakeIdleWorkers(int count) {
    // Use atomic array for lock-free wake - avoids mutex contention
    int wc = worker_count_.load(std::memory_order_acquire);
    int woken = 0;

    for (int i = 0; i < wc && woken < count; ++i) {
        Worker* w = workers_atomic_[i].load(std::memory_order_acquire);
        if (w) {
            w->WakeUp();
            ++woken;
        }
    }
}

} // namespace bthread