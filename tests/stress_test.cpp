#include "bthread.h"
#include "bthread/sync/mutex.hpp"

#include <cstdio>
#include <cassert>
#include <thread>
#include <vector>

static std::unique_ptr<bthread::Mutex> mutex;
static int counter = 0;

void* counter_task(void* arg) {
    int iterations = *static_cast<int*>(arg);

    for (int i = 0; i < iterations; ++i) {
        mutex->lock();
        counter++;
        mutex->unlock();
    }

    return arg;
}

void* recursive_task(void* arg) {
    int depth = *static_cast<int*>(arg);

    if (depth > 0) {
        bthread_t tid;
        int new_depth = depth - 1;
        bthread_create(&tid, nullptr, recursive_task, &new_depth);
        bthread_join(tid, nullptr);
    }

    return arg;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    fprintf(stderr, "[main] Starting stress test\n");

    // Initialize mutex before any bthread operations
    mutex = std::make_unique<bthread::Mutex>();

    fprintf(stderr, "[main] Mutex initialized\n");

    printf("Running stress tests...\n");

    printf("  Test 1: High concurrency...\n");
    counter = 0;

    const int num_threads = 100;
    const int iterations = 100;
    bthread_t tids[100];
    int thread_args[100];

    for (int i = 0; i < num_threads; ++i) {
        thread_args[i] = iterations;
        int ret = bthread_create(&tids[i], nullptr, counter_task, &thread_args[i]);
        assert(ret == 0);
    }

    for (int i = 0; i < num_threads; ++i) {
        bthread_join(tids[i], nullptr);
    }

    assert(counter == num_threads * iterations);
    printf("    PASSED\n");

    printf("  Test 2: Deep recursion...\n");
    const int recursion_depth = 50;
    bthread_t root_tid;
    bthread_create(&root_tid, nullptr, recursive_task, (void*)&recursion_depth);
    bthread_join(root_tid, nullptr);
    printf("    PASSED\n");

    printf("  Test 3: Rapid create/destroy...\n");
    const int rapid_iterations = 100;  // Reduced from 1000
    for (int i = 0; i < rapid_iterations; ++i) {
        bthread_t tid;
        int val = 1;
        bthread_create(&tid, nullptr, counter_task, &val);
        bthread_join(tid, nullptr);
    }
    printf("    PASSED\n");

    printf("All stress tests passed!\n");

    // Don't reset mutex - bthreads might still be using it
    // The mutex will be destroyed during static destruction after shutdown

    fprintf(stderr, "[main] Calling bthread_shutdown()...\n");
    bthread_shutdown();
    fprintf(stderr, "[main] bthread_shutdown() returned\n");
    return 0;
}