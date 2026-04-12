#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <mutex>

#include "bthread/platform/platform.h"
#include "bthread/pool/object_pool.hpp"

namespace bthread {

// Timer entry - with pool linkage
struct TimerEntry {
    std::atomic<TimerEntry*> pool_next{nullptr};  // Required by ObjectPool
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

    /// Initialize shards for worker count
    void Init(int worker_count);

private:
    // ========== Timer Sharding (Optimization 4) ==========
    static constexpr int MAX_SHARDS = 256;

    struct TimerShard {
        std::mutex mutex;                       // Per-shard lock
        std::vector<TimerEntry*> heap;          // Min-heap for this shard
        std::atomic<int64_t> next_deadline{INT64_MAX};  // Earliest deadline in shard
    };

    TimerShard shards_[MAX_SHARDS];
    std::atomic<int> shard_assign_{0};          // Round-robin shard assignment
    int worker_count_{0};                        // Number of shards to use

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

    void ProcessShard(TimerShard& shard);  // Process expired timers in one shard

    // Shard-specific heap helpers
    static void ShardSiftUp(std::vector<TimerEntry*>& heap, size_t idx);
    static void ShardSiftDown(std::vector<TimerEntry*>& heap, size_t idx);
    static void ShardPopFromHeap(std::vector<TimerEntry*>& heap);

    std::vector<TimerEntry*> heap_;
    std::mutex heap_mutex_;

    std::atomic<bool> running_{false};
    std::atomic<int> wakeup_futex_{0};  // For FutexWait (must be int)
    platform::ThreadId thread_;
    std::atomic<int> next_id_{0};

    // Object pool for TimerEntry
    static ObjectPool<TimerEntry> entry_pool_;
};

} // namespace bthread