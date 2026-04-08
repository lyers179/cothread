#include "bthread.h"
#include <thread>
#include <vector>
#include <cassert>
#include <cstdio>

static std::atomic<int> batch_counter{0};

void* batch_task(void* arg) {
    int n = *static_cast<int*>(arg);
    for (int i = 0; i < n; ++i) {
        bthread_yield();
    }
    batch_counter.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

int main() {
    printf("Testing Worker Batching...\n\n");

    // Test 1: Batch Boundaries
    printf("Test 1: Batch Boundaries\n");
    batch_counter = 0;

    const int N = 100;
    std::vector<bthread_t> tids(N);
    std::vector<int> args(N, 10);

    for (int i = 0; i < N; ++i) {
        bthread_create(&tids[i], nullptr, batch_task, &args[i]);
    }

    for (int i = 0; i < N; ++i) {
        bthread_join(tids[i], nullptr);
    }

    assert(batch_counter.load() == N);
    printf("  PASSED: All %d batched tasks completed\n", N);

    // Test 2: Stealing During Batch
    printf("\nTest 2: Stealing During Batch\n");
    batch_counter = 0;

    const int N2 = 200;
    std::vector<bthread_t> tids2(N2);
    std::vector<int> args2(N2, 5);

    for (int i = 0; i < N2; ++i) {
        bthread_create(&tids2[i], nullptr, batch_task, &args2[i]);
    }

    for (int i = 0; i < N2; ++i) {
        bthread_join(tids2[i], nullptr);
    }

    assert(batch_counter.load() == N2);
    printf("  PASSED: All %d batched tasks with work stealing completed\n", N2);

    // Shutdown at the end
    bthread_shutdown();

    printf("\nAll Worker Batch tests passed!\n");
    return 0;
}