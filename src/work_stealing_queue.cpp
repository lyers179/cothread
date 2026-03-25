#include "bthread/work_stealing_queue.h"

namespace bthread {

WorkStealingQueue::WorkStealingQueue() {
    for (size_t i = 0; i < CAPACITY; ++i) {
        buffer_[i].store(nullptr, std::memory_order_relaxed);
    }
}

void WorkStealingQueue::Push(TaskMeta* task) {
    uint64_t t = tail_.load(std::memory_order_relaxed);
    uint32_t idx = ExtractIndex(t);

    buffer_[idx].store(task, std::memory_order_relaxed);

    // Increment tail with version
    tail_.store(MakeVal(ExtractVersion(t) + 1, (idx + 1) % CAPACITY),
                std::memory_order_release);
}

TaskMeta* WorkStealingQueue::Pop() {
    uint64_t h = head_.load(std::memory_order_relaxed);
    uint64_t t = tail_.load(std::memory_order_acquire);

    if (ExtractIndex(h) == ExtractIndex(t)) {
        return nullptr;  // Empty
    }

    // Decrement tail first (LIFO for owner)
    uint32_t idx = (ExtractIndex(t) - 1 + CAPACITY) % CAPACITY;
    TaskMeta* task = buffer_[idx].load(std::memory_order_relaxed);

    if (idx == ExtractIndex(h)) {
        // Only one element, need to claim via head
        uint64_t expected = h;
        if (head_.compare_exchange_strong(expected,
                MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.store(MakeVal(ExtractVersion(t) + 1, idx), std::memory_order_release);
            return task;
        }
        return nullptr;
    }

    tail_.store(MakeVal(ExtractVersion(t) + 1, idx), std::memory_order_release);
    return task;
}

TaskMeta* WorkStealingQueue::Steal() {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);

    if (ExtractIndex(h) == ExtractIndex(t)) {
        return nullptr;  // Empty
    }

    uint32_t idx = ExtractIndex(h);
    TaskMeta* task = buffer_[idx].load(std::memory_order_acquire);

    // Try to claim this slot
    if (head_.compare_exchange_strong(h,
            MakeVal(ExtractVersion(h) + 1, (idx + 1) % CAPACITY),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return task;
    }

    return nullptr;
}

bool WorkStealingQueue::Empty() const {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);
    return ExtractIndex(h) == ExtractIndex(t);
}

} // namespace bthread