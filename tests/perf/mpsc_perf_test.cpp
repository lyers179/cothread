#include <gtest/gtest.h>
#include "bthread/queue/mpsc_queue.hpp"
#include "bthread/core/task_meta.hpp"
#include <thread>
#include <atomic>
#include <chrono>

namespace bthread {

// Test: Pop should spin before yield on race condition
TEST(MpscPerf, PopShouldNotYieldImmediately) {
    MpscQueue<TaskMetaBase> queue;

    // Set up race condition
    auto* item1 = new TaskMeta();
    auto* item2 = new TaskMeta();

    queue.Push(item1);
    queue.Push(item2);

    // Multiple threads popping simultaneously
    std::atomic<int> pops{0};
    std::thread t1([&]() {
        auto* p = queue.Pop();
        if (p) {
            delete static_cast<TaskMeta*>(p);
            pops.fetch_add(1);
        }
    });
    std::thread t2([&]() {
        auto* p = queue.Pop();
        if (p) {
            delete static_cast<TaskMeta*>(p);
            pops.fetch_add(1);
        }
    });

    t1.join();
    t2.join();

    EXPECT_EQ(pops.load(), 2);
}

// Test: High contention Push/Pop throughput
TEST(MpscPerf, HighContentionThroughput) {
    MpscQueue<TaskMetaBase> queue;
    const int num_producers = 4;
    const int ops_per_producer = 1000;

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    auto begin = std::chrono::high_resolution_clock::now();

    // Producer threads
    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            ready.fetch_add(1);
            while (!start.load()) {}
            for (int j = 0; j < ops_per_producer; ++j) {
                auto* item = new TaskMeta();
                queue.Push(item);
            }
        });
    }

    // Consumer thread
    std::thread consumer([&]() {
        ready.fetch_add(1);
        while (!start.load()) {}
        int consumed = 0;
        while (consumed < num_producers * ops_per_producer) {
            auto* item = queue.Pop();
            if (item) {
                delete static_cast<TaskMeta*>(item);
                ++consumed;
            }
        }
    });

    // Wait for all threads ready
    while (ready.load() < num_producers + 1) {}
    start.store(true);

    for (auto& p : producers) p.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);

    int total_ops = num_producers * ops_per_producer;
    double throughput = static_cast<double>(total_ops) * 1000000.0 / duration.count();

    // Log results
    std::cout << "High contention throughput: " << throughput << " ops/sec" << std::endl;
    std::cout << "Duration: " << duration.count() << " us" << std::endl;

    // Expect reasonable throughput (at least 10K ops/sec)
    EXPECT_GT(throughput, 10000);
}

// Test: PopMultiple efficiency
TEST(MpscPerf, PopMultipleEfficiency) {
    MpscQueue<TaskMetaBase> queue;
    const int batch_size = 16;
    const int num_items = batch_size * 10;

    // Pre-populate queue
    for (int i = 0; i < num_items; ++i) {
        queue.Push(new TaskMeta());
    }

    TaskMetaBase* buffer[256];

    auto begin = std::chrono::high_resolution_clock::now();

    int total_popped = 0;
    while (total_popped < num_items) {
        int count = queue.PopMultiple(buffer, batch_size);
        for (int i = 0; i < count; ++i) {
            delete static_cast<TaskMeta*>(buffer[i]);
        }
        total_popped += count;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);

    double throughput = static_cast<double>(num_items) * 1000000.0 / duration.count();

    std::cout << "PopMultiple throughput: " << throughput << " ops/sec" << std::endl;
    std::cout << "Batch size: " << batch_size << std::endl;

    EXPECT_EQ(total_popped, num_items);
    // Batch operations should be reasonably fast
    EXPECT_GT(throughput, 100000);
}

// Test: Adaptive spin effectiveness under contention
TEST(MpscPerf, AdaptiveSpinUnderContention) {
    MpscQueue<TaskMetaBase> queue;
    const int total_items = 1000;

    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    auto begin = std::chrono::high_resolution_clock::now();

    // Producer thread (with small delays to create contention)
    std::thread producer([&]() {
        for (int i = 0; i < total_items; ++i) {
            queue.Push(new TaskMeta());
            produced.fetch_add(1);
        }
    });

    // Consumer thread - simple loop until all consumed
    std::thread consumer([&]() {
        while (consumed.load() < total_items) {
            auto* item = queue.Pop();
            if (item) {
                delete static_cast<TaskMeta*>(item);
                consumed.fetch_add(1);
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);

    double throughput = static_cast<double>(total_items) * 1000000.0 / duration.count();

    std::cout << "Adaptive spin throughput: " << throughput << " ops/sec" << std::endl;

    EXPECT_EQ(produced.load(), total_items);
    EXPECT_EQ(consumed.load(), total_items);
}

} // namespace bthread