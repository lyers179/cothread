#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include "bthread/core/scheduler.hpp"

namespace bthread {
namespace test {

class IdleRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Don't call Init() here since we're testing the idle registry directly
        // and don't want worker threads running
    }

    void TearDown() override {
        // Reset idle state for next test
        Scheduler::Instance().ResetIdleRegistryForTest();
    }
};

TEST_F(IdleRegistryTest, PopFromEmptyReturnsMinusOne) {
    int id = Scheduler::Instance().PopIdleWorker();
    EXPECT_EQ(id, -1);
}

TEST_F(IdleRegistryTest, RegisterAndPopSingleWorker) {
    Scheduler::Instance().RegisterIdleWorkerForTest(0);

    int id = Scheduler::Instance().PopIdleWorker();
    EXPECT_EQ(id, 0);

    id = Scheduler::Instance().PopIdleWorker();
    EXPECT_EQ(id, -1);
}

TEST_F(IdleRegistryTest, RegisterMultipleWorkers) {
    Scheduler::Instance().RegisterIdleWorkerForTest(0);
    Scheduler::Instance().RegisterIdleWorkerForTest(1);
    Scheduler::Instance().RegisterIdleWorkerForTest(2);

    int count = 0;
    int ids[3];
    for (int i = 0; i < 3; ++i) {
        int id = Scheduler::Instance().PopIdleWorker();
        if (id >= 0) {
            ids[count++] = id;
        }
    }

    EXPECT_EQ(count, 3);
    // Verify all IDs are in valid range
    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(ids[i], 0);
        EXPECT_LT(ids[i], 3);
    }
}

TEST_F(IdleRegistryTest, ConcurrentRegisterAndPop) {
    std::atomic<int> registered_count{0};
    std::atomic<int> popped_count{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i]() {
            Scheduler::Instance().RegisterIdleWorkerForTest(i);
            registered_count.fetch_add(1);
        });
    }

    for (auto& t : threads) t.join();

    while (true) {
        int id = Scheduler::Instance().PopIdleWorker();
        if (id < 0) break;
        popped_count.fetch_add(1);
    }

    EXPECT_EQ(registered_count.load(), 4);
    EXPECT_EQ(popped_count.load(), 4);
}

TEST_F(IdleRegistryTest, PopAllAfterRegisterAll) {
    // Register multiple workers
    for (int i = 0; i < 10; ++i) {
        Scheduler::Instance().RegisterIdleWorkerForTest(i);
    }

    // Pop all and count
    int count = 0;
    while (Scheduler::Instance().PopIdleWorker() >= 0) {
        ++count;
    }

    EXPECT_EQ(count, 10);
}

} // namespace test
} // namespace bthread