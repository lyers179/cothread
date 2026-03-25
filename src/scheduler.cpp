#include "bthread/scheduler.h"
#include "bthread/task_group.h"
#include "bthread/timer_thread.h"
#include "bthread/butex.h"
#include "bthread/platform/platform.h"

#include <thread>

namespace bthread {

Scheduler::Scheduler() : task_group_(GetTaskGroup()) {}

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

        StartWorkers(n);
        task_group_.set_worker_count(n);
        initialized_.store(true, std::memory_order_release);
        running_.store(true, std::memory_order_release);
    });
}

void Scheduler::Shutdown() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);
    WakeAllWorkers();

    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto* w : workers_) {
        platform::JoinThread(w->thread());
        delete w;
    }
    workers_.clear();
    worker_count_.store(0, std::memory_order_release);
}

void Scheduler::StartWorkers(int count) {
    std::lock_guard<std::mutex> lock(workers_mutex_);

    workers_.reserve(count);
    for (int i = 0; i < count; ++i) {
        Worker* w = new Worker(i);
        w->thread = platform::CreateThread([](void* arg) {
            static_cast<Worker*>(arg)->Run();
        }, w);
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

void Scheduler::EnqueueTask(TaskMeta* task) {
    // First try to push to current worker's local queue
    Worker* current = Worker::Current();
    if (current) {
        current->local_queue().Push(task);
    } else {
        // Not in a worker thread, push to global queue
        global_queue().Push(task);
        WakeIdleWorkers(1);
    }
}

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