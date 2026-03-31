#include "bthread.h"
#include "bthread/mutex.h"
#include <cstdio>

static bthread_mutex_t test_mutex;
static int counter = 0;

void* simple_mutex_task(void* arg) {
    int id = (int)(intptr_t)arg;
    printf("Task %d: starting\n", id);

    for (int i = 0; i < 10; ++i) {
        bthread_mutex_lock(&test_mutex);
        counter++;
        bthread_mutex_unlock(&test_mutex);
    }

    printf("Task %d: finished\n", id);
    return arg;
}

int main() {
    printf("=== Simple Mutex Test ===\n");

    bthread_mutex_init(&test_mutex, nullptr);
    counter = 0;

    bthread_t tids[2];
    for (int i = 0; i < 2; ++i) {
        int ret = bthread_create(&tids[i], nullptr, simple_mutex_task, (void*)(intptr_t)i);
        printf("Created bthread %d, ret=%d\n", i, ret);
    }

    printf("Joining...\n");
    for (int i = 0; i < 2; ++i) {
        bthread_join(tids[i], nullptr);
        printf("Joined bthread %d\n", i);
    }

    printf("Counter = %d (expected 20)\n", counter);
    bthread_mutex_destroy(&test_mutex);

    printf("Shutting down...\n");
    bthread_shutdown();

    printf("=== Test Complete ===\n");
    return 0;
}
