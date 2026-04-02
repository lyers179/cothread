#include "bthread/scheduler.h"
#include "bthread/task_group.h"
#include "bthread/timer_thread.h"
#include "bthread/butex.h"
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

    // Stop timer thread first to prevent new timer callbacks
    if (timer_thread_) {
        timer_thread_->Stop();
    }

    // Signal all workers to stop
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto* w : workers_) {
            w->Stop();
        }
    }

    // Wait and re-signal workers
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto* w : workers_) {
            w->Stop();
        }
    }

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
    std::lock_guard<std::mutex> lock(workers_mutex_);
    if (index >= 0 && index < static_cast<int>(workers_.size())) {
        return workers_[index];
    }
    return nullptr;
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
    } else {
        // Not in a worker thread, push to global queue
        global_queue_.Push(task);
        // Wake up all idle workers to ensure tasks are processed
        WakeIdleWorkers(worker_count_.load(std::memory_order_acquire));
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
    std::lock_guard<std::mutex> lock(workers_mutex_);
    int woken = 0;
    for (auto* w : workers_) {
        if (woken >= count) break;
        w->WakeUp();
        ++woken;
    }
}

} // namespace bthread