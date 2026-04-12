#include "bthread/queue/shared_mpsc_queue.hpp"

namespace bthread {

void SharedMPSCQueue::Init(int worker_count) {
    worker_count_ = worker_count;
}

void SharedMPSCQueue::Push(TaskMetaBase* task) {
    // Round-robin distribution to shards
    int shard = round_robin_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
    shards_[shard].Push(task);
    // Optimization 1: Increment total count for O(1) Empty check
    total_count_.fetch_add(1, std::memory_order_release);
}

TaskMetaBase* SharedMPSCQueue::Pop(int worker_id) {
    // 1. Try own shard first (fast path)
    if (worker_id >= 0 && worker_id < worker_count_) {
        TaskMetaBase* task = shards_[worker_id].Pop();
        if (task) {
            // Optimization 1: Decrement total count
            total_count_.fetch_sub(1, std::memory_order_release);
            return task;
        }
    }

    // 2. Steal from other shards (slow path)
    for (int i = 0; i < worker_count_; ++i) {
        if (i == worker_id) continue;
        TaskMetaBase* task = shards_[i].Pop();
        if (task) {
            // Optimization 1: Decrement total count
            total_count_.fetch_sub(1, std::memory_order_release);
            return task;
        }
    }

    return nullptr;
}

bool SharedMPSCQueue::Empty() const {
    // Optimization 1: O(1) check using atomic counter
    // Note: May have brief inconsistency (task added but Pop returned it)
    // This is acceptable as Empty() is only used for quick checks
    return total_count_.load(std::memory_order_acquire) == 0;
}

} // namespace bthread