#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "bthread/queue/sharded_queue.hpp"
#include "bthread/core/task_meta.hpp"

namespace bthread {
namespace test {

class ShardedQueueTest : public ::testing::Test {
protected:
    ShardedGlobalQueue queue_;

    void SetUp() override {
        queue_.Init(4);
    }
};

TEST_F(ShardedQueueTest, PushPopSingleTask) {
    TaskMeta task;
    queue_.Push(&task);

    TaskMetaBase* popped = queue_.Pop(0);
    EXPECT_EQ(popped, &task);

    EXPECT_TRUE(queue_.Empty());
}

TEST_F(ShardedQueueTest, RoundRobinDistribution) {
    TaskMeta tasks[8];

    for (int i = 0; i < 8; ++i) {
        queue_.Push(&tasks[i]);
    }

    int counts[4] = {0, 0, 0, 0};
    for (int shard = 0; shard < 4; ++shard) {
        while (TaskMetaBase* t = queue_.Pop(shard)) {
            counts[shard]++;
        }
    }

    int total = 0;
    for (int c : counts) total += c;
    EXPECT_EQ(total, 8);
}

TEST_F(ShardedQueueTest, StealFromOtherShard) {
    TaskMeta task;
    queue_.Push(&task);

    TaskMetaBase* popped = queue_.Pop(1);
    EXPECT_EQ(popped, &task);
}

TEST_F(ShardedQueueTest, ConcurrentPushPop) {
    std::atomic<int> pushed{0};
    std::atomic<int> popped{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < 100; ++i) {
                TaskMeta* task = new TaskMeta();
                queue_.Push(task);
                pushed.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < 4; ++c) {
        consumers.emplace_back([&, c]() {
            for (int i = 0; i < 100; ++i) {
                if (TaskMetaBase* t = queue_.Pop(c)) {
                    popped.fetch_add(1);
                    delete static_cast<TaskMeta*>(t);
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(pushed.load(), 400);
    EXPECT_GT(popped.load(), 300);

    for (int i = 0; i < 4; ++i) {
        while (TaskMetaBase* t = queue_.Pop(i)) {
            delete static_cast<TaskMeta*>(t);
        }
    }
}

} // namespace test
} // namespace bthread