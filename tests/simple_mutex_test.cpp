#include "bthread.h"
#include "bthread/mutex.h"

#include <cstdio>
#include <cassert>

static int counter = 0;
static bthread_mutex_t mutex;

void* simple_task(void* arg) {
    int iterations = *static_cast<int*>(arg);

    for (int i = 0; i < iterations; ++i) {
        int ret = bthread_mutex_lock(&mutex);
        if (ret != 0) {
            fprintf(stderr, "Lock failed: %d\n", ret);
            return nullptr;
        }
        counter++;
        ret = bthread_mutex_unlock(&mutex);
        if (ret != 0) {
            fprintf(stderr, "Unlock failed: %d\n", ret);
            return nullptr;
        }
    }

    return nullptr;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    fprintf(stderr, "Simple mutex test...\n");

    bthread_mutex_init(&mutex, nullptr);
    counter = 0;

    const int num_threads = 2;
    const int iterations = 100;
    bthread_t tids[2];
    int args[2];

    for (int i = 0; i < num_threads; ++i) {
        args[i] = iterations;
        int ret = bthread_create(&tids[i], nullptr, simple_task, &args[i]);
        fprintf(stderr, "Created bthread %d, ret=%d\n", i, ret);
    }

    fprintf(stderr, "Joining...\n");

    for (int i = 0; i < num_threads; ++i) {
        fprintf(stderr, "Joining bthread %d...\n", i);
        int ret = bthread_join(tids[i], nullptr);
        fprintf(stderr, "Joined bthread %d, ret=%d\n", i, ret);
    }

    fprintf(stderr, "Counter: %d (expected: %d)\n", counter, num_threads * iterations);

    bthread_mutex_destroy(&mutex);

    fprintf(stderr, "Test done!\n");
    return 0;
}