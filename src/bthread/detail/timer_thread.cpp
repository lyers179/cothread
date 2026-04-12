#include "bthread/detail/timer_thread.hpp"
#include "bthread/platform/platform.h"
#include "bthread/core/scheduler.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace bthread {

TimerThread::TimerThread() = default;

TimerThread::~TimerThread() {
    Stop();
}

void TimerThread::Init(int worker_count) {
    worker_count_ = std::min(worker_count, MAX_SHARDS);
    if (worker_count_ <= 0) {
        worker_count_ = 1;  // At least 1 shard
    }
}

void TimerThread::Start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    thread_ = platform::CreateThread([](void* arg) {
        static_cast<TimerThread*>(arg)->TimerThreadMain();
    }, this);
}

void TimerThread::Stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(false, std::memory_order_release);
    // Wake up timer thread from FutexWait
    wakeup_futex_.fetch_add(1, std::memory_order_release);
    platform::FutexWake(&wakeup_futex_, 1);
    platform::JoinThread(thread_);

    // Clean up remaining entries in shards
    if (worker_count_ > 0) {
        for (int i = 0; i < worker_count_; ++i) {
            std::lock_guard<std::mutex> lock(shards_[i].mutex);
            for (auto* entry : shards_[i].heap) {
                delete entry;
            }
            shards_[i].heap.clear();
        }
    } else {
        // Fallback to global heap
        std::lock_guard<std::mutex> lock(heap_mutex_);
        for (auto* entry : heap_) {
            delete entry;
        }
        heap_.clear();
    }
}

int TimerThread::Schedule(void (*callback)(void*), void* arg, const platform::timespec* delay) {
    if (!delay) return -1;

    // Calculate deadline
    int64_t delay_us = static_cast<int64_t>(delay->tv_sec) * 1000000 +
                       delay->tv_nsec / 1000;
    int64_t deadline_us = platform::GetTimeOfDayUs() + delay_us;

    // Create entry
    auto* entry = new TimerEntry();
    entry->callback = callback;
    entry->arg = arg;
    entry->deadline_us = deadline_us;
    entry->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    entry->cancelled = false;

    // Use sharded scheduling if initialized
    if (worker_count_ > 0) {
        // Round-robin shard assignment
        int shard_id = shard_assign_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
        TimerShard& shard = shards_[shard_id];

        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.heap.push_back(entry);
        ShardSiftUp(shard.heap, shard.heap.size() - 1);

        // Update next_deadline atomically
        if (entry->deadline_us < shard.next_deadline.load(std::memory_order_acquire)) {
            shard.next_deadline.store(entry->deadline_us, std::memory_order_release);
        }
    } else {
        // Fallback to global heap
        std::lock_guard<std::mutex> lock(heap_mutex_);
        AddToHeap(entry);
    }

    return entry->id;
}

bool TimerThread::Cancel(int timer_id) {
    // Search shards if initialized
    if (worker_count_ > 0) {
        for (int i = 0; i < worker_count_; ++i) {
            std::lock_guard<std::mutex> lock(shards_[i].mutex);
            for (auto* entry : shards_[i].heap) {
                if (entry->id == timer_id) {
                    entry->cancelled = true;
                    return true;
                }
            }
        }
        return false;
    }

    // Fallback to global heap
    std::lock_guard<std::mutex> lock(heap_mutex_);
    for (auto* entry : heap_) {
        if (entry->id == timer_id) {
            entry->cancelled = true;
            return true;
        }
    }
    return false;
}

void TimerThread::TimerThreadMain() {
    while (running_.load(std::memory_order_acquire)) {
        int64_t now_us = platform::GetTimeOfDayUs();
        int64_t min_deadline = INT64_MAX;

        if (worker_count_ > 0) {
            // Sharded mode: check all shards atomically first
            for (int i = 0; i < worker_count_; ++i) {
                int64_t shard_deadline = shards_[i].next_deadline.load(std::memory_order_acquire);
                if (shard_deadline < min_deadline) {
                    min_deadline = shard_deadline;
                }

                // Process expired timers in this shard
                if (shard_deadline <= now_us) {
                    ProcessShard(shards_[i]);
                    // Update min_deadline after processing
                    int64_t new_deadline = shards_[i].next_deadline.load(std::memory_order_acquire);
                    if (new_deadline < min_deadline) {
                        min_deadline = new_deadline;
                    }
                }
            }
        } else {
            // Fallback to global heap
            std::lock_guard<std::mutex> lock(heap_mutex_);

            // Process expired timers
            while (!heap_.empty()) {
                TimerEntry* entry = heap_[0];

                if (entry->cancelled) {
                    // Remove cancelled entry
                    PopFromHeap();
                    delete entry;
                    continue;
                }

                if (entry->deadline_us > now_us) {
                    min_deadline = entry->deadline_us;
                    break;
                }

                // Timer expired, pop and execute
                PopFromHeap();
                void (*callback)(void*) = entry->callback;
                void* arg = entry->arg;
                delete entry;

                // Execute callback (release lock first)
                heap_mutex_.unlock();
                callback(arg);
                heap_mutex_.lock();
            }
        }

        // Calculate sleep time
        int64_t sleep_us = min_deadline - now_us;
        if (sleep_us <= 0 || min_deadline == INT64_MAX) {
            sleep_us = 100000;  // Default 100ms sleep
        }

        // Sleep
        platform::timespec ts;
        ts.tv_sec = sleep_us / 1000000;
        ts.tv_nsec = (sleep_us % 1000000) * 1000;

        // Use dedicated futex for proper wakeups
        platform::FutexWait(&wakeup_futex_, wakeup_futex_.load(std::memory_order_acquire), &ts);
    }
}

void TimerThread::ProcessShard(TimerShard& shard) {
    int64_t now_us = platform::GetTimeOfDayUs();

    std::unique_lock<std::mutex> lock(shard.mutex);

    while (!shard.heap.empty() && shard.heap[0]->deadline_us <= now_us) {
        TimerEntry* entry = shard.heap[0];

        if (entry->cancelled) {
            ShardPopFromHeap(shard.heap);
            delete entry;
            continue;
        }

        // Pop from heap
        ShardPopFromHeap(shard.heap);

        // Update next_deadline
        if (!shard.heap.empty()) {
            shard.next_deadline.store(shard.heap[0]->deadline_us, std::memory_order_release);
        } else {
            shard.next_deadline.store(INT64_MAX, std::memory_order_release);
        }

        // Execute callback (release lock)
        void (*callback)(void*) = entry->callback;
        void* arg = entry->arg;
        delete entry;

        shard.mutex.unlock();
        callback(arg);
        shard.mutex.lock();
    }

    // Update next_deadline if heap is not empty but no timers expired
    if (!shard.heap.empty()) {
        shard.next_deadline.store(shard.heap[0]->deadline_us, std::memory_order_release);
    }
}

void TimerThread::AddToHeap(TimerEntry* entry) {
    heap_.push_back(entry);
    SiftUp(heap_.size() - 1);
}

TimerEntry* TimerThread::PopFromHeap() {
    if (heap_.empty()) {
        return nullptr;
    }

    TimerEntry* root = heap_[0];
    heap_[0] = heap_.back();
    heap_.pop_back();

    if (!heap_.empty()) {
        SiftDown(0);
    }

    return root;
}

void TimerThread::RemoveFromHeap(TimerEntry* entry) {
    // Find entry
    auto it = std::find(heap_.begin(), heap_.end(), entry);
    if (it == heap_.end()) {
        return;
    }

    size_t idx = it - heap_.begin();

    // Replace with last element
    heap_[idx] = heap_.back();
    heap_.pop_back();

    // Restore heap property
    if (idx > 0 && heap_[idx]->deadline_us < heap_[(idx - 1) / 2]->deadline_us) {
        SiftUp(idx);
    } else {
        SiftDown(idx);
    }
}

void TimerThread::SiftDown(size_t idx) {
    size_t n = heap_.size();

    while (true) {
        size_t left = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        size_t smallest = idx;

        if (left < n && heap_[left]->deadline_us < heap_[smallest]->deadline_us) {
            smallest = left;
        }
        if (right < n && heap_[right]->deadline_us < heap_[smallest]->deadline_us) {
            smallest = right;
        }

        if (smallest == idx) {
            break;
        }

        std::swap(heap_[idx], heap_[smallest]);
        idx = smallest;
    }
}

void TimerThread::SiftUp(size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;

        if (heap_[parent]->deadline_us <= heap_[idx]->deadline_us) {
            break;
        }

        std::swap(heap_[idx], heap_[parent]);
        idx = parent;
    }
}

// Shard-specific heap helpers (static)
void TimerThread::ShardSiftUp(std::vector<TimerEntry*>& heap, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;

        if (heap[parent]->deadline_us <= heap[idx]->deadline_us) {
            break;
        }

        std::swap(heap[idx], heap[parent]);
        idx = parent;
    }
}

void TimerThread::ShardSiftDown(std::vector<TimerEntry*>& heap, size_t idx) {
    size_t n = heap.size();

    while (true) {
        size_t left = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        size_t smallest = idx;

        if (left < n && heap[left]->deadline_us < heap[smallest]->deadline_us) {
            smallest = left;
        }
        if (right < n && heap[right]->deadline_us < heap[smallest]->deadline_us) {
            smallest = right;
        }

        if (smallest == idx) {
            break;
        }

        std::swap(heap[idx], heap[smallest]);
        idx = smallest;
    }
}

void TimerThread::ShardPopFromHeap(std::vector<TimerEntry*>& heap) {
    if (heap.empty()) {
        return;
    }

    heap[0] = heap.back();
    heap.pop_back();

    if (!heap.empty()) {
        ShardSiftDown(heap, 0);
    }
}

} // namespace bthread