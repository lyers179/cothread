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

Scheduler::Scheduler() {
    // Ensure TaskGroup is constructed BEFORE Scheduler so it's destroyed AFTER
    // This is critical because Scheduler's destructor calls GetTaskGroup()
    (void)GetTaskGroup();
}

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
        global_queue_.Init(n);

        // Wait for all workers to be ready before returning
        {
            std::unique_lock<std::mutex> lock(workers_ready_mutex_);
            workers_ready_cv_.wait(lock, [this, n] {
                return workers_ready_.load(std::memory_order_acquire) >= n;
            });
        }

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
            // Initialize with worker count before starting
            // Use configured_count_ if available, otherwise use actual worker_count_
            int wc = configured_count_ > 0 ? configured_count_ : worker_count_.load(std::memory_order_acquire);
            if (wc <= 0) {
                wc = std::thread::hardware_concurrency();
                if (wc == 0) wc = 4;
            }
            timer_thread_->Init(wc);
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

void Scheduler::RegisterIdleWorker(int worker_id) {
    // Push worker_id onto idle list (lock-free Treiber stack)
    int old_head = idle_head_.load(std::memory_order_acquire);
    do {
        idle_next_[worker_id].store(old_head, std::memory_order_relaxed);
    } while (!idle_head_.compare_exchange_weak(old_head, worker_id,
            std::memory_order_release, std::memory_order_acquire));
}

int Scheduler::PopIdleWorker() {
    // Pop one worker from idle list (lock-free)
    int worker_id = idle_head_.load(std::memory_order_acquire);
    while (worker_id >= 0) {
        int next = idle_next_[worker_id].load(std::memory_order_relaxed);
        if (idle_head_.compare_exchange_weak(worker_id, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return worker_id;  // Successfully popped
        }
        // CAS failed, retry with new head
    }
    return -1;  // No idle workers
}

void Scheduler::WakeIdleWorkers(int count) {
    // Optimized: Only wake N idle workers (not ALL workers)
    for (int i = 0; i < count; ++i) {
        int idle_id = PopIdleWorker();
        if (idle_id < 0) break;  // No idle workers available

        Worker* w = workers_atomic_[idle_id].load(std::memory_order_acquire);
        if (w) {
            w->WakeUp();
        }
    }
}

void Scheduler::WorkerReady() {
    int prev = workers_ready_.fetch_add(1, std::memory_order_acq_rel);
    // Notify on every increment (simple but slightly inefficient)
    // Could optimize to only notify when reaching expected count
    workers_ready_cv_.notify_one();
}

TaskMetaBase* Scheduler::PopGlobal(int worker_id) {
    return global_queue_.Pop(worker_id);
}

// ========== Test Helper Methods ==========

void Scheduler::RegisterIdleWorkerForTest(int worker_id) {
    RegisterIdleWorker(worker_id);
}

void Scheduler::ResetIdleRegistryForTest() {
    idle_head_.store(-1, std::memory_order_release);
}

} // namespace bthread