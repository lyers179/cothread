#include "bthread/sync/butex_queue.hpp"
#include "bthread/sync/butex.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/queue/execution_queue.hpp"
#include "bthread.h"
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

void test_butex_concurrent_wake() {
    printf("\nTest: Butex Concurrent Wake (Lock-Free)\n");

    // Test multiple threads calling Wake concurrently
    // This verifies that PopFromHead is truly MPMC-safe without mutex protection
    const int N = 10;  // Number of waiters
    const int WAKERS = 4;  // Number of concurrent wake threads

    Butex butex;
    std::atomic<int> wait_ready{0};
    std::atomic<int> woken_count{0};
    std::atomic<bool> start_wake{false};

    // Structure to pass to bthread
    struct WaiterArgs {
        Butex* butex;
        std::atomic<int>* woken_count;
        std::atomic<int>* wait_ready;
    };

    std::vector<bthread_t> tids(N);
    std::vector<WaiterArgs> args(N);

    // Create N bthreads that will wait on the butex
    for (int i = 0; i < N; ++i) {
        args[i].butex = &butex;
        args[i].woken_count = &woken_count;
        args[i].wait_ready = &wait_ready;
        bthread_create(&tids[i], nullptr, [](void* arg) -> void* {
            auto* wa = static_cast<WaiterArgs*>(arg);
            wa->wait_ready->fetch_add(1, std::memory_order_release);
            wa->butex->Wait(0, nullptr);
            wa->woken_count->fetch_add(1, std::memory_order_release);
            return nullptr;
        }, &args[i]);
    }

    // Wait for all bthreads to be ready (they've incremented wait_ready and are about to wait)
    while (wait_ready.load(std::memory_order_acquire) < N) {
        std::this_thread::yield();
    }

    // Give bthreads time to actually call Wait and block
    usleep(10000);  // 10ms

    // Launch WAKERS threads that will concurrently call Wake
    std::vector<std::thread> wakers;
    for (int i = 0; i < WAKERS; ++i) {
        wakers.emplace_back([&] {
            // Wait for start signal
            while (!start_wake.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // Each wakes up to 3 waiters
            butex.Wake(3);
        });
    }

    // Start all wakers at once
    start_wake.store(true, std::memory_order_release);

    // Wait for all wakers
    for (auto& w : wakers) w.join();

    // Wait for all bthreads to complete
    for (int i = 0; i < N; ++i) {
        bthread_join(tids[i], nullptr);
    }

    printf("  Waiters: %d, Wakers: %d, Woken: %d\n", N, WAKERS, woken_count.load());
    assert(woken_count.load() == N);
    printf("  PASSED: Concurrent Wake test completed without deadlock or crash\n");
}

void test_execution_queue_concurrent() {
    printf("\nTest: ExecutionQueue Concurrent Submit\n");

    bthread::ExecutionQueue queue;
    std::atomic<int> executed{0};
    std::atomic<int> submitted{0};  // Track submitted tasks
    std::atomic<bool> producers_done{false};

    const int PRODUCERS = 10;
    const int TASKS_PER_PRODUCER = 100;
    const int TOTAL_TASKS = PRODUCERS * TASKS_PER_PRODUCER;

    // Producer threads submit tasks concurrently
    std::vector<std::thread> producers;
    for (int i = 0; i < PRODUCERS; ++i) {
        producers.emplace_back([&] {
            for (int j = 0; j < TASKS_PER_PRODUCER; ++j) {
                queue.Submit([&executed] {
                    executed.fetch_add(1, std::memory_order_relaxed);
                });
                submitted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Single consumer thread executes tasks
    std::thread consumer([&] {
        while (executed.load(std::memory_order_acquire) < TOTAL_TASKS) {
            if (!queue.ExecuteOne()) {
                // If queue is empty but producers are done, we might have a race
                // Wait a bit for tasks to appear
                std::this_thread::yield();
            }
        }
    });

    // Wait for all producers
    for (auto& p : producers) p.join();
    producers_done.store(true, std::memory_order_release);

    // Wait for consumer to finish
    consumer.join();

    printf("  Producers: %d, Tasks per producer: %d, Submitted: %d, Executed: %d (expected: %d)\n",
           PRODUCERS, TASKS_PER_PRODUCER, submitted.load(), executed.load(), TOTAL_TASKS);
    assert(submitted.load() == TOTAL_TASKS);
    assert(executed.load() == TOTAL_TASKS);
    printf("  PASSED: All tasks submitted and executed correctly\n");
}

void test_pop_no_timeout_when_nonempty() {
    printf("\nTest: Pop No Timeout When Queue Nonempty\n");

    // This test verifies that PopFromHead does not return nullptr when nodes exist
    // or are being added to the queue. The bug was: timeout returns nullptr even
    // when queue has nodes, causing Wake to miss waiters.

    ButexQueue queue;
    std::atomic<bool> pop_started{false};
    std::atomic<bool> node_added{false};
    std::atomic<TaskMeta*> popped{nullptr};

    TaskMeta task;
    task.is_waiting.store(true, std::memory_order_relaxed);
    task.butex_waiter_node.claimed.store(false, std::memory_order_relaxed);
    task.butex_waiter_node.next.store(nullptr, std::memory_order_relaxed);

    std::thread popper([&] {
        pop_started.store(true);
        // Wait until node is added to queue
        while (!node_added.load()) {
            std::this_thread::yield();
        }
        // PopFromHead should return the task, not nullptr
        TaskMeta* result = queue.PopFromHead();
        popped.store(result);
    });

    // Wait for popper thread to start
    while (!pop_started.load()) {
        std::this_thread::yield();
    }

    // Small delay to ensure popper is running
    usleep(1000);  // 1ms

    // Add task to queue
    queue.AddToTail(&task);
    node_added.store(true);

    popper.join();

    TaskMeta* result = popped.load();
    printf("  Expected task ptr: %p, Got: %p\n", (void*)&task, (void*)result);
    assert(result == &task && "PopFromHead should not return nullptr when queue has nodes");
    printf("  PASSED: PopFromHead returns task when queue nonempty\n");
}

int main() {
    printf("Testing MPMC Queue PopFromHead Implementation...\n\n");

    test_mpmc_concurrent_pop();
    test_mpmc_interleaved_push_pop();
    test_mpmc_no_double_pop();
    test_pop_no_timeout_when_nonempty();
    test_butex_concurrent_wake();
    test_execution_queue_concurrent();

    printf("\nMPMC Queue test passed!\n");
    return 0;
}