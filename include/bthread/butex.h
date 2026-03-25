#pragma once

#include <atomic>
#include <cstdint>

#include "bthread/task_meta.h"
#include "bthread/platform/platform.h"

namespace bthread {

// Butex - binaryutex for bthread synchronization
class Butex {
public:
    Butex();
    ~Butex();

    // Disable copy and move
    Butex(const Butex&) = delete;
    Butex& operator=(const Butex&) = delete;

    // Wait until value != expected_value
    // Returns 0 on success, ETIMEDOUT on timeout
    int Wait(int expected_value, const platform::timespec* timeout);

    // Wake up to 'count' waiters
    void Wake(int count);

    // Get/set value
    int value() const { return value_.load(std::memory_order_acquire); }
    void set_value(int v) { value_.store(v, std::memory_order_release); }

private:
    // Remove waiter from queue
    void RemoveFromWaitQueue(TaskMeta* waiter);

    // Timeout callback
    static void TimeoutCallback(void* arg);

    std::atomic<TaskMeta*> waiters_{nullptr};
    std::atomic<int> value_{0};
};

} // namespace bthread