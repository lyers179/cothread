#include "bthread/sync/butex_queue.hpp"
#include "bthread/core/task_meta.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <cstdio>

using namespace bthread;

void test_mpmc_concurrent_pop() {
    printf("Test: MPMC Concurrent Pop\n");

    ButexQueue queue;
    std::atomic<int> popped_count{0};
    std::atomic<bool> done{false};
    std::atomic<bool> start{false};

    // Create 100 tasks
    std::vector<TaskMeta> tasks(100);
    for (auto& t : tasks) {
        t.is_waiting.store(true, std::memory_order_relaxed);
        t.butex_waiter_node.claimed.store(false, std::memory_order_relaxed);
        queue.AddToTail(&t);
    }

    // Launch 4 consumer threads
    std::vector<std::thread> consumers;
    for (int i =  0; i < 4; ++i) {
        consumers.emplace_back([&] {
            // Wait for start signal
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!done.load(std::memory_order_acquire)) {
                TaskMeta* t = queue.PopFromHead();
                if (t) {
                    popped_count.fetch_add(1, std::memory_order_relaxed);
                }
                if (popped_count.load(std::memory_order_acquire) >= 100) {
                    done.store(true, std::memory_order_release);
                }
            }
        });
    }

    // Start all consumers at once
    start.store(true, std::memory_order_release);

    // Wait for all consumers
    for (auto& c : consumers) c.join();

    printf("  Popped count: %d (expected: 100)\n", popped_count.load());
    assert(popped_count.load() == 100);
    printf("  PASSED: All tasks popped exactly once\n");
}

void test_mpmc_interleaved_push_pop() {
    printf("\nTest: MPMC Interleaved Push and Pop\n");

    ButexQueue queue;
    std::atomic<int> pushed_count{0};
    std::atomic<int> popped_count{0};
    std::atomic<bool> done{false};
    std::atomic<bool> start{false};

    const int N = 1000;  // Total tasks
    const int PRODUCERS = 2;
    const int CONSUMERS = 4;

    // Pre-allocate task pool
    std::vector<TaskMeta> tasks(N);

    // Producer threads
    std::vector<std::thread> producers;
    for (int i = 0; i < PRODUCERS; ++i) {
        producers.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            int base = i * (N / PRODUCERS);
            int count = N / PRODUCERS;
            for (int j = 0; j < count; ++j) {
                TaskMeta& t = tasks[base + j];
                t.is_waiting.store(true, std::memory_order_relaxed);
                t.butex_waiter_node.claimed.store(false, std::memory_order_relaxed);
                queue.AddToTail(&t);
                pushed_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Consumer threads
    std::vector<std::thread> consumers;
    for (int i = 0; i < CONSUMERS; ++i) {
        consumers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!done.load(std::memory_order_acquire)) {
                TaskMeta* t = queue.PopFromHead();
                if (t) {
                    popped_count.fetch_add(1, std::memory_order_relaxed);
                }
                // Check if all tasks have been pushed and popped
                if (pushed_count.load(std::memory_order_acquire) >= N &&
                    popped_count.load(std::memory_order_acquire) >= N) {
                    done.store(true, std::memory_order_release);
                    break;
                }
                // Safety: if we've popped everything pushed, we're done
                int pushed = pushed_count.load(std::memory_order_acquire);
                int popped = popped_count.load(std::memory_order_acquire);
                if (pushed >= N && popped >= pushed) {
                    done.store(true, std::memory_order_release);
                    break;
                }
            }
        });
    }

    // Start all threads at once
    start.store(true, std::memory_order_release);

    // Wait for all producers
    for (auto& p : producers) p.join();

    // Wait for all consumers
    for (auto& c : consumers) c.join();

    printf("  Pushed: %d, Popped: %d (expected: %d)\n",
           pushed_count.load(), popped_count.load(), N);
    assert(pushed_count.load() == N);
    assert(popped_count.load() == N);
    printf("  PASSED: All tasks pushed and popped correctly\n");
}

void test_mpmc_no_double_pop() {
    printf("\nTest: MPMC No Double Pop\n");

    ButexQueue queue;
    std::atomic<int> popped_count{0};
    std::vector<std::atomic<bool>> popped_tasks(100);
    for (auto& p : popped_tasks) {
        p.store(false, std::memory_order_relaxed);
    }

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};

    // Create 100 tasks
    std::vector<TaskMeta> tasks(100);
    for (int i = 0; i < 100; ++i) {
        tasks[i].is_waiting.store(true, std::memory_order_relaxed);
        tasks[i].butex_waiter_node.claimed.store(false, std::memory_order_relaxed);
        // Store index in the task for tracking
        tasks[i].ref_count.store(i, std::memory_order_relaxed);  // Reuse ref_count as index
        queue.AddToTail(&tasks[i]);
    }

    // Consumer threads
    std::vector<std::thread> consumers;
    for (int i = 0; i < 4; ++i) {
        consumers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!done.load(std::memory_order_acquire)) {
                TaskMeta* t = queue.PopFromHead();
                if (t) {
                    int idx = t->ref_count.load(std::memory_order_relaxed);
                    // Check if this task was already popped
                    bool expected = false;
                    if (popped_tasks[idx].compare_exchange_strong(expected, true,
                            std::memory_order_acq_rel)) {
                        popped_count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // Double pop detected!
                        printf("  ERROR: Task %d was popped twice!\n", idx);
                        assert(false && "Double pop detected");
                    }
                }
                if (popped_count.load(std::memory_order_acquire) >= 100) {
                    done.store(true, std::memory_order_release);
                }
            }
        });
    }

    // Start all consumers at once
    start.store(true, std::memory_order_release);

    // Wait for all consumers
    for (auto& c : consumers) c.join();

    printf("  Popped count: %d (expected: 100)\n", popped_count.load());
    assert(popped_count.load() == 100);
    printf("  PASSED: No task was popped more than once\n");
}

int main() {
    printf("Testing MPMC Queue PopFromHead Implementation...\n\n");

    test_mpmc_concurrent_pop();
    test_mpmc_interleaved_push_pop();
    test_mpmc_no_double_pop();

    printf("\nMPMC Queue test passed!\n");
    return 0;
}