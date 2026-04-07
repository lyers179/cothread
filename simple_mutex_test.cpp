#include "bthread.h"
#include "bthread/sync/mutex.hpp"
#include <stdio.h>
#include <atomic>

std::atomic<int> counter{0};
bthread::Mutex mtx;

void* increment_task(void* arg) {
    int n = *static_cast<int*>(arg);
    for (int i = 0; i < n; ++i) {
        mtx.lock();
        counter++;
        mtx.unlock();
    }
    printf("Task done, counter=%d\n", counter.load());
    return nullptr;
}

int main() {
    printf("Starting simple mutex test...\n");

    const int N = 2;
    bthread_t tids[N];
    int args[N] = {10, 10};

    for (int i = 0; i < N; ++i) {
        printf("Creating thread %d\n", i);
        int ret = bthread_create(&tids[i], nullptr, increment_task, &args[i]);
        printf("Create thread %d: ret=%d\n", i, ret);
    }

    for (int i = 0; i < N; ++i) {
        printf("Joining thread %d\n", i);
        bthread_join(tids[i], nullptr);
        printf("Thread %d joined\n", i);
    }

    printf("Final counter: %d (expected: %d)\n", counter.load(), N * 10);
    printf("Test %s\n", counter.load() == N * 10 ? "PASSED" : "FAILED");

    return 0;
}