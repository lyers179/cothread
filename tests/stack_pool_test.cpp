#include <gtest/gtest.h>
#include "bthread/core/worker.hpp"
#include "bthread/platform/platform.h"

using namespace bthread;

// Test fixture for stack pool tests
class StackPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize scheduler if needed
    }

    void TearDown() override {
        // Cleanup
    }
};

// Test basic stack reuse - same stack returned after release
TEST_F(StackPoolTest, BasicReuse) {
    Worker w(0);

    // Acquire a stack
    void* s1 = w.AcquireStack();
    ASSERT_NE(s1, nullptr);

    // Release it back to pool
    w.ReleaseStack(s1, Worker::DEFAULT_STACK_SIZE);

    // Acquire again - should get same stack
    void* s2 = w.AcquireStack();
    EXPECT_EQ(s1, s2) << "Stack should be reused from pool";

    // Cleanup
    w.ReleaseStack(s2, Worker::DEFAULT_STACK_SIZE);
    w.DrainStackPool();
}

// Test pool fills up and falls back to direct allocation
TEST_F(StackPoolTest, PoolFullFallback) {
    Worker w(0);
    void* stacks[Worker::STACK_POOL_SIZE + 2];

    // Fill the pool by acquiring more than pool size
    for (int i = 0; i < Worker::STACK_POOL_SIZE + 1; ++i) {
        stacks[i] = w.AcquireStack();
        ASSERT_NE(stacks[i], nullptr);
    }

    // Verify we got distinct stacks
    for (int i = 0; i < Worker::STACK_POOL_SIZE; ++i) {
        EXPECT_NE(stacks[i], stacks[Worker::STACK_POOL_SIZE])
            << "Stack " << i << " should differ from overflow stack";
    }

    // Release all stacks
    for (int i = 0; i < Worker::STACK_POOL_SIZE + 1; ++i) {
        w.ReleaseStack(stacks[i], Worker::DEFAULT_STACK_SIZE);
    }

    // Pool should be full (STACK_POOL_SIZE items)
    EXPECT_EQ(w.stack_pool_count(), Worker::STACK_POOL_SIZE);

    // Cleanup
    w.DrainStackPool();
}

// Test that releasing to full pool deallocates
TEST_F(StackPoolTest, ReleaseWhenPoolFull) {
    Worker w(0);
    void* stacks[Worker::STACK_POOL_SIZE];

    // Fill pool
    for (int i = 0; i < Worker::STACK_POOL_SIZE; ++i) {
        stacks[i] = w.AcquireStack();
    }

    // Release all - pool should be full after first STACK_POOL_SIZE releases
    for (int i = 0; i < Worker::STACK_POOL_SIZE; ++i) {
        w.ReleaseStack(stacks[i], Worker::DEFAULT_STACK_SIZE);
    }

    EXPECT_EQ(w.stack_pool_count(), Worker::STACK_POOL_SIZE);

    // Cleanup
    w.DrainStackPool();
    EXPECT_EQ(w.stack_pool_count(), 0);
}

// Test shutdown cleanup
TEST_F(StackPoolTest, ShutdownCleanup) {
    Worker w(0);
    void* stacks[Worker::STACK_POOL_SIZE];

    // Acquire all stacks first
    for (int i = 0; i < Worker::STACK_POOL_SIZE; ++i) {
        stacks[i] = w.AcquireStack();
    }

    // Release all to fill pool
    for (int i = 0; i < Worker::STACK_POOL_SIZE; ++i) {
        w.ReleaseStack(stacks[i], Worker::DEFAULT_STACK_SIZE);
    }

    EXPECT_EQ(w.stack_pool_count(), Worker::STACK_POOL_SIZE);

    // Drain should clear pool
    w.DrainStackPool();
    EXPECT_EQ(w.stack_pool_count(), 0);
}

// Test custom stack size falls back to direct allocation
TEST_F(StackPoolTest, CustomStackSize) {
    Worker w(0);

    // Request larger stack - should not use pool
    void* s1 = w.AcquireStack(16384);  // 16KB instead of default 8KB
    ASSERT_NE(s1, nullptr);

    // Release should not pool (wrong size) - it deallocates directly
    w.ReleaseStack(s1, 16384);
    EXPECT_EQ(w.stack_pool_count(), 0) << "Wrong size stack should not be pooled";

    // No manual deallocation needed - ReleaseStack already freed it
}
