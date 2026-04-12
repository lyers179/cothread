#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>
#include "bthread/queue/mpsc_queue.hpp"
#include "bthread/core/task_meta_base.hpp"

using bthread::WaiterQueue;
using bthread::TaskMetaBase;
using bthread::TaskState;

// Test TaskMetaBase subclass for testing
struct TestTask : TaskMetaBase {
    TestTask() : TaskMetaBase() {
        type = bthread::TaskType::BTHREAD;
    }
    void resume() override {}
};

TEST(WaiterQueueTest, SinglePushPop) {
    WaiterQueue queue;
    TestTask task;

    EXPECT_TRUE(queue.Empty());

    queue.Push(&task);
    EXPECT_FALSE(queue.Empty());

    TaskMetaBase* popped = queue.Pop();
    EXPECT_EQ(popped, &task);
    EXPECT_TRUE(queue.Empty());
}

TEST(WaiterQueueTest, MultiplePushPopFIFO) {
    WaiterQueue queue;
    std::vector<std::unique_ptr<TestTask>> tasks;

    // Create tasks
    for (int i = 0; i < 10; ++i) {
        tasks.push_back(std::make_unique<TestTask>());
    }

    // Push in order
    for (int i = 0; i < 10; ++i) {
        queue.Push(tasks[i].get());
    }

    // Pop should return in FIFO order
    for (int i = 0; i < 10; ++i) {
        TaskMetaBase* popped = queue.Pop();
        EXPECT_EQ(popped, tasks[i].get());
    }
    EXPECT_TRUE(queue.Empty());
}

TEST(WaiterQueueTest, MultiProducerSingleConsumer) {
    WaiterQueue queue;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int TASKS_PER_PRODUCER = 100;
    std::vector<std::vector<std::unique_ptr<TestTask>>> producer_tasks(NUM_PRODUCERS);

    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        for (int i = 0; i < TASKS_PER_PRODUCER; ++i) {
            producer_tasks[p].push_back(std::make_unique<TestTask>());
        }
    }

    std::atomic<int> push_count{0};
    std::atomic<bool> done_pushing{false};

    // Producers
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &producer_tasks, p, &push_count]() {
            for (int i = 0; i < TASKS_PER_PRODUCER; ++i) {
                queue.Push(producer_tasks[p][i].get());
                push_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Consumer
    std::atomic<int> pop_count{0};
    std::thread consumer([&queue, &push_count, &done_pushing, &pop_count, NUM_PRODUCERS, TASKS_PER_PRODUCER]() {
        while (pop_count.load() < NUM_PRODUCERS * TASKS_PER_PRODUCER) {
            TaskMetaBase* task = queue.Pop();
            if (task) {
                pop_count.fetch_add(1, std::memory_order_relaxed);
            } else if (done_pushing.load()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (auto& t : producers) {
        t.join();
    }
    done_pushing.store(true);

    consumer.join();

    EXPECT_EQ(push_count.load(), NUM_PRODUCERS * TASKS_PER_PRODUCER);
    EXPECT_EQ(pop_count.load(), NUM_PRODUCERS * TASKS_PER_PRODUCER);
}

TEST(WaiterQueueTest, WaiterNextFieldClear) {
    WaiterQueue queue;
    TestTask task;

    queue.Push(&task);
    // waiter_next should be nullptr after push (task is at tail)
    EXPECT_EQ(task.waiter_next.load(), nullptr);

    TestTask task2;
    queue.Push(&task2);
    // Now task's waiter_next should point to task2
    EXPECT_EQ(task.waiter_next.load(), &task2);

    TaskMetaBase* popped = queue.Pop();
    EXPECT_EQ(popped, &task);
    // popped task's waiter_next should be cleared
    EXPECT_EQ(task.waiter_next.load(), nullptr);
}