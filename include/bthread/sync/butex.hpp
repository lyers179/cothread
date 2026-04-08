#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "bthread/sync/butex_queue.hpp"
#include "bthread/platform/platform.h"

namespace bthread {

// Forward declarations
class Worker;
struct TaskMeta;

// Butex - binary mutex for bthread synchronization
// Supports FIFO (append) and LIFO (prepend) wait queue ordering
class Butex {
public:
    Butex();
    ~Butex();

    // Disable copy and move
    Butex(const Butex&) = delete;
    Butex& operator=(const Butex&) = delete;

    // Wait until value != expected_value
    // prepend: if true, add to head (LIFO); if false, add to tail (FIFO)
    // Returns 0 on success, ETIMEDOUT on timeout
    int Wait(int expected_value, const platform::timespec* timeout, bool prepend = false);

    // Wake up to 'count' waiters (always from head - FIFO order for fairness)
    void Wake(int count);

    // Get/set value
    int value() const { return value_.load(std::memory_order_acquire); }
    void set_value(int v) { value_.store(v, std::memory_order_release); }

    // Expose queue for timeout callback (internal use)
    ButexQueue& queue() { return queue_; }

private:
    ButexQueue queue_;           // Lock-free MPSC queue for waiters
    std::atomic<int> value_{0};  // Current value

    // Timeout callback
    static void TimeoutCallback(void* arg);
};

} // namespace bthread