#include "bthread.h"
#include "bthread/core/scheduler.hpp"
#include "bthread/core/task_meta.hpp"
#include "bthread/core/task_group.hpp"
#include "bthread/platform/platform.h"

#include <cstdio>

void* simple_task(void* arg) {
    fprintf(stderr, "    Inside bthread! arg=%p\n", arg);
    fflush(stderr);
    return arg;
}

int main() {
    // Disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "=== Scheduler Test (stderr) ===\n");
    fflush(stderr);

    fprintf(stderr, "1. Initializing scheduler...\n");
    fflush(stderr);
    bthread::Scheduler::Instance().Init();
    fprintf(stderr, "   Worker count: %d\n", bthread::Scheduler::Instance().worker_count());
    fflush(stderr);

    fprintf(stderr, "2. Creating bthread...\n");
    fflush(stderr);
    bthread_t tid;
    int ret = bthread_create(&tid, nullptr, simple_task, (void*)0x1234);
    fprintf(stderr, "   bthread_create returned %d, tid=%llu\n", ret, (unsigned long long)tid);
    fflush(stderr);

    if (ret != 0) {
        fprintf(stderr, "   FAILED: create returned error\n");
        fflush(stderr);
        return 1;
    }

    fprintf(stderr, "3. Joining bthread...\n");
    fflush(stderr);
    void* result = nullptr;
    ret = bthread_join(tid, &result);
    fprintf(stderr, "   bthread_join returned %d, result=%p\n", ret, result);
    fflush(stderr);

    fprintf(stderr, "4. Test completed!\n");
    fflush(stderr);
    bthread_shutdown();
    return 0;
}