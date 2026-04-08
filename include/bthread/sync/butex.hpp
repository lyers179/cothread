#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "bthread/core/task_meta.hpp"
#include "bthread/platform/platform.h"

namespace bthread {

// Forward declarations
class Worker;
struct TaskMeta;

// ButexWaiterNode forward declaration
struct ButexWaiterNode;

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

private:
    // Lock-free MPSC queue
    std::atomic<ButexWaiterNode*> head_{nullptr};
    std::atomic<ButexWaiterNode*> tail_{nullptr};
    std::atomic<int> value_{0};

    // Add waiter to head (LIFO) - used for re-queueing woken threads
    void AddToHead(TaskMeta* waiter);

    // Add waiter to tail (FIFO) - used for first-time waiters
    void AddToTail(TaskMeta* waiter);

    // Remove waiter from queue
    void RemoveFromWaitQueue(TaskMeta* waiter);

    // Pop waiter from head
    TaskMeta* PopFromHead();

    // Timeout callback
    static void TimeoutCallback(void* arg);
};

} // namespace bthread