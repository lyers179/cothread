#include "bthread.h"
#include "bthread/sync/mutex.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <cstdio>
#include "bthread/core/scheduler.hpp"

using namespace bthread;

// Test counter - atomics for thread safety
static std::atomic<int> test_counter{0};

void* simple_task(void* arg) {
    test_counter.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void* mutex_task(void* arg) {
    auto* p = static_cast<std::pair<Mutex*, int>*>(arg);
    for (int i = 0; i < p->second; ++i) {
        p->first->lock();
        test_counter.fetch_add(1, std::memory_order_relaxed);
        p->first->unlock();
    }
    return nullptr;
}

int main() {
    printf("Testing MPSC Queue Lock-Free Implementation...\n\n");

    // Test 1: Single Producer Single Consumer
    printf("Test 1: Single Producer Single Consumer\n");
    test_counter = 0;

    const int N = 1000;
    std::vector<bthread_t> tids(N);

    for (int i = 0; i < N; ++i) {
        bthread_create(&tids[i], nullptr, simple_task, nullptr);
    }

    for (int i = 0; i < N; ++i) {
        bthread_join(tids[i], nullptr);
    }

    assert(test_counter.load() == N);
    printf("  PASSED: All %d tasks executed\n", N);

    // Test 2: High Contention Mutex
    printf("\nTest 2: High Contention Mutex\n");
    test_counter = 0;

    const int N2 = 10000;
    const int THREADS = 16;

    Mutex mtx;
    std::vector<bthread_t> tids2(THREADS);

    // Allocate args array to avoid dangling pointers
    std::vector<std::pair<Mutex*, int>> args_array;
    for (int i = 0; i < THREADS; ++i) {
        args_array.push_back({&mtx, N2 / THREADS});
    }

    for (int i = 0; i < THREADS; ++i) {
        bthread_create(&tids2[i], nullptr, mutex_task, &args_array[i]);
    }

    for (int i = 0; i < THREADS; ++i) {
        bthread_join(tids2[i], nullptr);
    }

    assert(test_counter.load() == N2);
    printf("  PASSED: High contention test completed\n", N2);

    printf("\nAll MPSC Queue tests passed!\n");
    bthread_shutdown();
    return 0;
}