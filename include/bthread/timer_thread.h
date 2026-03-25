#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <mutex>

#include "bthread/platform/platform.h"

namespace bthread {

// Timer entry
struct TimerEntry {
    void (*callback)(void*);
    void* arg;
    int64_t deadline_us;
    int id;
    bool cancelled;
};

// Min-heap based timer
class TimerThread {
public:
    TimerThread();
    ~TimerThread();

    // Disable copy and move
    TimerThread(const TimerThread&) = delete;
    TimerThread& operator=(const TimerThread&) = delete;

    // Start timer thread
    void Start();

    // Stop timer thread
    void Stop();

    // Schedule a timer callback
    // Returns timer ID (>= 0) on success, -1 on failure
    int Schedule(void (*callback)(void*), void* arg, const platform::timespec* delay);

    // Cancel a timer
    // Returns true if cancelled, false if not found or already executed
    bool Cancel(int timer_id);

    // Check if running
    bool running() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    // Timer thread main loop
    void TimerThreadMain();

    // Add entry to heap
    void AddToHeap(TimerEntry* entry);

    // Pop earliest entry from heap
    TimerEntry* PopFromHeap();

    // Remove entry from heap
    void RemoveFromHeap(TimerEntry* entry);

    // Heap operations
    void SiftDown(size_t idx);
    void SiftUp(size_t idx);

    std::vector<TimerEntry*> heap_;
    std::mutex heap_mutex_;

    std::atomic<bool> running_{false};
    platform::ThreadId thread_;
    std::atomic<int> next_id_{0};
};

} // namespace bthread