#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "bthread/pool/object_pool.hpp"

using bthread::ObjectPool;

// Test object with required pool_next field
struct TestObject {
    std::atomic<TestObject*> pool_next{nullptr};
    int value{0};

    TestObject() = default;
    explicit TestObject(int v) : value(v) {}
};

TEST(ObjectPoolTest, AcquireReleaseBasic) {
    ObjectPool<TestObject> pool(8);

    EXPECT_EQ(pool.size(), 0);

    TestObject* obj = pool.Acquire();
    EXPECT_NE(obj, nullptr);
    EXPECT_EQ(pool.size(), 0);  // Acquire decrements pool

    pool.Release(obj);
    EXPECT_EQ(pool.size(), 1);  // Release adds to pool
}

TEST(ObjectPoolTest, PoolReuse) {
    ObjectPool<TestObject> pool(8);

    // Acquire and release same object multiple times
    TestObject* obj1 = pool.Acquire();
    obj1->value = 42;
    pool.Release(obj1);

    TestObject* obj2 = pool.Acquire();
    EXPECT_EQ(obj2->value, 42);  // Should be same object (LIFO)
    EXPECT_EQ(obj1, obj2);

    pool.Release(obj2);
}

TEST(ObjectPoolTest, PoolSizeLimit) {
    ObjectPool<TestObject> pool(4);

    // Create 6 objects
    std::vector<TestObject*> objects;
    for (int i = 0; i < 6; ++i) {
        objects.push_back(new TestObject(i));
    }

    // Release all - only 4 should be pooled
    for (auto* obj : objects) {
        pool.Release(obj);
    }

    EXPECT_EQ(pool.size(), 4);  // Pool limit enforced

    // Acquire 4 - should get pooled objects
    for (int i = 0; i < 4; ++i) {
        TestObject* obj = pool.Acquire();
        EXPECT_NE(obj, nullptr);
        EXPECT_EQ(pool.size(), 4 - i - 1);
    }

    EXPECT_EQ(pool.size(), 0);
}

TEST(ObjectPoolTest, MultiThreadedAcquireRelease) {
    ObjectPool<TestObject> pool(32);
    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 1000;

    std::atomic<int> total_acquired{0};
    std::atomic<int> total_released{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&pool, &total_acquired, &total_released]() {
            std::vector<TestObject*> local_objects;
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                TestObject* obj = pool.Acquire();
                total_acquired.fetch_add(1, std::memory_order_relaxed);
                local_objects.push_back(obj);

                // Occasionally release some back
                if (local_objects.size() > 10) {
                    pool.Release(local_objects.back());
                    local_objects.pop_back();
                    total_released.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Release remaining
            for (auto* obj : local_objects) {
                pool.Release(obj);
                total_released.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(total_acquired.load(), NUM_THREADS * OPS_PER_THREAD);
    EXPECT_EQ(total_released.load(), NUM_THREADS * OPS_PER_THREAD);
    EXPECT_LE(pool.size(), pool.max_size());
}

TEST(ObjectPoolTest, DestructorDrainsPool) {
    TestObject* obj = nullptr;

    {
        ObjectPool<TestObject> pool(8);
        obj = pool.Acquire();
        pool.Release(obj);
        EXPECT_EQ(pool.size(), 1);
        // Destructor should delete pooled objects
    }

    // obj was deleted by pool destructor - we can't safely check
    // but no crash means success
}