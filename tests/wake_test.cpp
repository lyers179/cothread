#include "bthread.h"
#include "bthread/scheduler.h"
#include "bthread/worker.h"

#include <cstdio>

void* simple_task(void* arg) {
    int id = (int)(intptr_t)arg;
    fprintf(stderr, "    Task %d running on worker %d\n", id,
           bthread::Worker::Current() ? bthread::Worker::Current()->id() : -1);
    fflush(stderr);
    return arg;
}

int main() {
    // Disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "=== Wake Test ===\n");

    fprintf(stderr, "1. Initializing scheduler...\n");
    fflush(stderr);
    bthread::Scheduler::Instance().Init();
    fprintf(stderr, "   Worker count: %d\n", bthread::Scheduler::Instance().worker_count());
    fflush(stderr);

    fprintf(stderr, "2. Creating 4 bthreads...\n");
    fflush(stderr);
    bthread_t tids[4];
    for (int i = 0; i < 4; ++i) {
        bthread_create(&tids[i], nullptr, simple_task, (void*)(intptr_t)i);
        fprintf(stderr, "   Created bthread %d, tid=%llu\n", i, (unsigned long long)tids[i]);
        fflush(stderr);
    }

    fprintf(stderr, "3. Joining...\n");
    fflush(stderr);
    for (int i = 0; i < 4; ++i) {
        bthread_join(tids[i], nullptr);
        fprintf(stderr, "   Joined bthread %d\n", i);
        fflush(stderr);
    }

    fprintf(stderr, "=== Test PASSED ===\n");
    fflush(stderr);
    return 0;
}