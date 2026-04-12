#pragma once

#include <atomic>
#include <cstdint>
#include "bthread/queue/mpsc_queue.hpp"
#include "bthread/core/task_meta_base.hpp"

namespace bthread {

/**
 * @brief Sharded global queue for MPMC task distribution.
 *
 * Each worker has its own shard (MpscQueue). Push uses round-robin.
 * Pop tries own shard first, then steals from other shards.
 *
 * Thread Safety:
 * - Push(): Safe from multiple producer threads (round-robin to shards)
 * - Pop(): Safe from multiple consumer threads (each pops from own shard, steals from others)
 */
class ShardedGlobalQueue {
public:
    static constexpr int MAX_SHARDS = 256;

    ShardedGlobalQueue() = default;
    ~ShardedGlobalQueue() = default;

    // Disable copy and move
    ShardedGlobalQueue(const ShardedGlobalQueue&) = delete;
    ShardedGlobalQueue& operator=(const ShardedGlobalQueue&) = delete;

    /**
     * @brief Initialize shards for worker count.
     * @param worker_count Number of workers/shards
     */
    void Init(int worker_count);

    /**
     * @brief Push task to queue (round-robin to shards).
     * @param task Task to push
     */
    void Push(TaskMetaBase* task);

    /**
     * @brief Pop task from queue (own shard first, then steal).
     * @param worker_id Worker ID to pop from
     * @return Task pointer, or nullptr if empty
     */
    TaskMetaBase* Pop(int worker_id);

    /**
     * @brief Check if all shards are empty.
     * @return true if no tasks in any shard
     */
    bool Empty() const;

private:
    std::atomic<int> round_robin_{0};
    int worker_count_{0};
    MpscQueue<TaskMetaBase> shards_[MAX_SHARDS];
};

} // namespace bthread