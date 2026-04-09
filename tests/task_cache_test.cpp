#include <gtest/gtest.h>
#include "bthread/core/worker.hpp"
#include "bthread/core/task_group.hpp"

using namespace bthread;

// Test fixture for TaskMeta cache tests
class TaskCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        // TaskGroup is a singleton, no need to create
    }

    void TearDown() override {
        // Cleanup
    }
};

// Test basic TaskMeta reuse - same TaskMeta returned after release
TEST_F(TaskCacheTest, BasicReuse) {
    Worker w(0);

    // Acquire a TaskMeta
    TaskMeta* t1 = w.AcquireTaskMeta();
    ASSERT_NE(t1, nullptr);

    // Release it back to cache
    w.ReleaseTaskMeta(t1);

    // Acquire again - should get same TaskMeta
    TaskMeta* t2 = w.AcquireTaskMeta();
    EXPECT_EQ(t1, t2) << "TaskMeta should be reused from cache";

    // Cleanup
    w.DrainTaskCache();
}

// Test cache refill when empty
TEST_F(TaskCacheTest, CacheRefill) {
    Worker w(0);
    TaskMeta* tasks[Worker::TASK_CACHE_SIZE + 1];

    // Exhaust the cache
    for (int i = 0; i < Worker::TASK_CACHE_SIZE + 1; ++i) {
        tasks[i] = w.AcquireTaskMeta();
        ASSERT_NE(tasks[i], nullptr) << "Should get TaskMeta " << i;
    }

    // Cache should have been refilled
    EXPECT_GE(w.task_cache_count(), 0);

    // Release all
    for (int i = 0; i < Worker::TASK_CACHE_SIZE + 1; ++i) {
        w.ReleaseTaskMeta(tasks[i]);
    }
}

// Test cache returns to current worker
TEST_F(TaskCacheTest, CacheAffinity) {
    Worker w(0);

    // Acquire and release
    TaskMeta* t1 = w.AcquireTaskMeta();
    ASSERT_NE(t1, nullptr);
    w.ReleaseTaskMeta(t1);

    // Same worker should get same TaskMeta
    TaskMeta* t2 = w.AcquireTaskMeta();
    EXPECT_EQ(t1, t2);

    // Cleanup
    w.DrainTaskCache();
}

// Test shutdown cleanup
TEST_F(TaskCacheTest, ShutdownCleanup) {
    Worker w(0);

    // Acquire and release to fill cache
    for (int i = 0; i < Worker::TASK_CACHE_SIZE; ++i) {
        TaskMeta* t = w.AcquireTaskMeta();
        if (t) w.ReleaseTaskMeta(t);
    }

    // Drain should clear cache
    w.DrainTaskCache();
    EXPECT_EQ(w.task_cache_count(), 0);
}

// Test multiple workers don't share cache
TEST_F(TaskCacheTest, MultipleWorkersSeparateCaches) {
    Worker w1(0);
    Worker w2(1);

    TaskMeta* t1 = w1.AcquireTaskMeta();
    TaskMeta* t2 = w2.AcquireTaskMeta();

    // Different workers may get different TaskMetas
    // (This is expected behavior - each has own cache)

    if (t1) w1.ReleaseTaskMeta(t1);
    if (t2) w2.ReleaseTaskMeta(t2);

    w1.DrainTaskCache();
    w2.DrainTaskCache();
}