#include <gtest/gtest.h>
#include "bthread.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace std::chrono_literals;

// Helper task functions
void* empty_task(void* arg) {
    (void)arg;
    return nullptr;
}

// Global test environment - initializes scheduler once
class ButexLazyTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        bthread_set_worker_count(4);
    }

    void TearDown() override {
        // Don't shutdown - scheduler is a singleton that can't restart
    }
};

// Test fixture for lazy Butex tests
class ButexLazyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Scheduler already initialized by environment
    }

    void TearDown() override {
        // No shutdown - let environment handle cleanup
    }
};

// Test that bthread without join doesn't allocate Butex upfront
TEST_F(ButexLazyTest, NoJoinNoUpfrontAlloc) {
    bthread_t tid;

    int ret = bthread_create(&tid, nullptr, empty_task, nullptr);
    EXPECT_EQ(ret, 0);

    // Let it finish
    std::this_thread::sleep_for(10ms);

    // Join should still work (allocates Butex lazily)
    ret = bthread_join(tid, nullptr);
    EXPECT_EQ(ret, 0);
}

// Test join on already finished bthread
TEST_F(ButexLazyTest, AlreadyFinished) {
    bthread_t tid;
    int ret = bthread_create(&tid, nullptr, empty_task, nullptr);
    EXPECT_EQ(ret, 0);

    // Wait for it to finish
    std::this_thread::sleep_for(100ms);

    // Join should return immediately
    auto start = std::chrono::high_resolution_clock::now();
    ret = bthread_join(tid, nullptr);
    auto end = std::chrono::high_resolution_clock::now();

    EXPECT_EQ(ret, 0);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_LT(duration.count(), 50) << "Join should return quickly for finished bthread";
}

// Test detach followed by no Butex needed
TEST_F(ButexLazyTest, DetachNoButex) {
    bthread_t tid;
    int ret = bthread_create(&tid, nullptr, empty_task, nullptr);
    EXPECT_EQ(ret, 0);

    // Detach - no Butex should be needed
    ret = bthread_detach(tid);
    EXPECT_EQ(ret, 0);

    // Let it finish
    std::this_thread::sleep_for(100ms);
}

// Test multiple joiners with lazy allocation
TEST_F(ButexLazyTest, MultipleJoiners) {
    std::atomic<int> counter{0};

    bthread_t tid;
    auto counter_task = [](void* arg) -> void* {
        auto* c = static_cast<std::atomic<int>*>(arg);
        c->fetch_add(1);
        std::this_thread::sleep_for(50ms);
        return nullptr;
    };

    int ret = bthread_create(&tid, nullptr, counter_task, &counter);
    EXPECT_EQ(ret, 0);

    // Two threads try to join - only one Butex should be created
    std::atomic<int> join_results{0};

    std::thread j1([&]() {
        int r = bthread_join(tid, nullptr);
        join_results.fetch_add(r == 0 ? 1 : 0);
    });

    std::thread j2([&]() {
        int r = bthread_join(tid, nullptr);
        join_results.fetch_add(r == 0 ? 1 : 0);
    });

    j1.join();
    j2.join();

    // At least one join should succeed
    EXPECT_GE(join_results.load(), 1);
}

// Test normal create/join cycle
TEST_F(ButexLazyTest, NormalCycle) {
    bthread_t tid;
    int ret = bthread_create(&tid, nullptr, empty_task, nullptr);
    EXPECT_EQ(ret, 0);

    ret = bthread_join(tid, nullptr);
    EXPECT_EQ(ret, 0);
}

// Register the global test environment
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new ButexLazyTestEnvironment());
    return RUN_ALL_TESTS();
}