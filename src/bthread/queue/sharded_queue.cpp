#include "bthread/queue/sharded_queue.hpp"

namespace bthread {

void ShardedGlobalQueue::Init(int worker_count) {
    worker_count_ = worker_count;
}

void ShardedGlobalQueue::Push(TaskMetaBase* task) {
    // Round-robin distribution to shards
    int shard = round_robin_.fetch_add(1, std::memory_order_relaxed) % worker_count_;
    shards_[shard].Push(task);
}

TaskMetaBase* ShardedGlobalQueue::Pop(int worker_id) {
    // 1. Try own shard first (fast path)
    if (worker_id >= 0 && worker_id < worker_count_) {
        TaskMetaBase* task = shards_[worker_id].Pop();
        if (task) return task;
    }

    // 2. Steal from other shards (slow path)
    for (int i = 0; i < worker_count_; ++i) {
        if (i == worker_id) continue;
        TaskMetaBase* task = shards_[i].Pop();
        if (task) return task;
    }

    return nullptr;
}

bool ShardedGlobalQueue::Empty() const {
    for (int i = 0; i < worker_count_; ++i) {
        if (!shards_[i].Empty()) return false;
    }
    return true;
}

} // namespace bthread