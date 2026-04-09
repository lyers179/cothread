// src/bthread/queue/work_stealing_queue.cpp
#include "bthread/queue/work_stealing_queue.hpp"

namespace bthread {

WorkStealingQueue::WorkStealingQueue() {
    for (size_t i = 0; i < CAPACITY; ++i) {
        buffer_[i].store(nullptr, std::memory_order_relaxed);
    }
}

void WorkStealingQueue::Push(TaskMetaBase* task) {
    uint64_t t = tail_.load(std::memory_order_relaxed);
    uint32_t idx = ExtractIndex(t);

    buffer_[idx].store(task, std::memory_order_relaxed);

    // Increment tail with version
    tail_.store(MakeVal(ExtractVersion(t) + 1, (idx + 1) % CAPACITY),
                std::memory_order_release);
}

TaskMetaBase* WorkStealingQueue::Pop() {
    uint64_t h = head_.load(std::memory_order_relaxed);
    uint64_t t = tail_.load(std::memory_order_acquire);

    if (ExtractIndex(h) == ExtractIndex(t)) {
        return nullptr;  // Empty
    }

    // Decrement tail first (LIFO for owner)
    uint32_t idx = (ExtractIndex(t) - 1 + CAPACITY) % CAPACITY;
    TaskMetaBase* task = buffer_[idx].load(std::memory_order_relaxed);

    if (idx == ExtractIndex(h)) {
        // Only one element, need to claim via head
        uint64_t expected = h;
        if (head_.compare_exchange_strong(expected,
                MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Set tail to match head (both point to next available slot)
            tail_.store(MakeVal(ExtractVersion(t) + 1, (idx + 1) % CAPACITY),
                        std::memory_order_release);
            return task;
        }
        return nullptr;
    }

    tail_.store(MakeVal(ExtractVersion(t) + 1, idx), std::memory_order_release);
    return task;
}

TaskMetaBase* WorkStealingQueue::Steal() {
    // Use CAS weak + retry for better performance under contention
    constexpr int MAX_STEAL_ATTEMPTS = 3;

    for (int attempt = 0; attempt < MAX_STEAL_ATTEMPTS; ++attempt) {
        uint64_t h = head_.load(std::memory_order_acquire);
        uint64_t t = tail_.load(std::memory_order_acquire);

        if (ExtractIndex(h) == ExtractIndex(t)) {
            return nullptr;  // Empty
        }

        uint32_t idx = ExtractIndex(h);
        TaskMetaBase* task = buffer_[idx].load(std::memory_order_acquire);

        // Use weak CAS - faster under contention, will retry on failure
        if (head_.compare_exchange_weak(h,
                MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return task;
        }
        // CAS failed due to contention, retry
    }

    return nullptr;
}

bool WorkStealingQueue::Empty() const {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);
    return ExtractIndex(h) == ExtractIndex(t);
}

int WorkStealingQueue::PopMultiple(TaskMetaBase** buffer, int max_count) {
    int count = 0;
    while (count < max_count) {
        TaskMetaBase* task = Pop();
        if (!task) break;
        buffer[count++] = task;
    }
    return count;
}

} // namespace bthread