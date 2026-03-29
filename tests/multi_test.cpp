#include "bthread.h"
#include "bthread/mutex.h"

#include <cstdio>
#include <cassert>

static int counter = 0;
static bthread_mutex_t mutex;

void* increment_task(void* arg) {
    int iterations = *static_cast<int*>(arg);
    fprintf(stderr, "    bthread started, iterations=%d\n", iterations);
    fflush(stderr);

    for (int i = 0; i < iterations; ++i) {
        bthread_mutex_lock(&mutex);
        counter++;
        bthread_mutex_unlock(&mutex);
    }

    fprintf(stderr, "    bthread finished\n");
    fflush(stderr);
    return arg;
}

int main() {
    // Disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "=== Multi-bthread Test ===\n");

    fprintf(stderr, "1. Initializing mutex...\n");
    fflush(stderr);
    bthread_mutex_init(&mutex, nullptr);

    fprintf(stderr, "2. Creating 2 bthreads...\n");
    fflush(stderr);
    counter = 0;

    const int iterations = 100;
    bthread_t tids[2];
    int thread_args[2] = {iterations, iterations};

    for (int i = 0; i < 2; ++i) {
        int ret = bthread_create(&tids[i], nullptr, increment_task, &thread_args[i]);
        fprintf(stderr, "   Created bthread %d: tid=%llu, ret=%d\n", i, (unsigned long long)tids[i], ret);
        fflush(stderr);
        assert(ret == 0);
    }

    fprintf(stderr, "3. Joining bthreads...\n");
    fflush(stderr);
    for (int i = 0; i < 2; ++i) {
        fprintf(stderr, "   Joining bthread %d...\n", i);
        fflush(stderr);
        int ret = bthread_join(tids[i], nullptr);
        fprintf(stderr, "   Joined bthread %d: ret=%d\n", i, ret);
        fflush(stderr);
    }

    fprintf(stderr, "4. Counter = %d (expected %d)\n", counter, 2 * iterations);
    fflush(stderr);
    assert(counter == 2 * iterations);

    bthread_mutex_destroy(&mutex);
    fprintf(stderr, "=== Test PASSED ===\n");
    fflush(stderr);
    return 0;
}