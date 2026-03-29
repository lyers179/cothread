#include "bthread.h"
#include "bthread/scheduler.h"

#include <cstdio>
#include <cstdlib>

int main() {
    // Disable buffering for stderr
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "=== Step 1: Pre-Init Check ===\n");
    fflush(stderr);

    fprintf(stderr, "=== Step 2: Calling Scheduler::Instance().Init() ===\n");
    fflush(stderr);
    bthread::Scheduler::Instance().Init();

    fprintf(stderr, "=== Step 3: Scheduler Initialized ===\n");
    fflush(stderr);

    fprintf(stderr, "=== Step 4: Creating bthread ===\n");
    fflush(stderr);
    bthread_t tid;
    int ret = bthread_create(&tid, nullptr, [](void* arg) -> void* {
        fprintf(stderr, "=== Inside bthread ===\n");
        fflush(stderr);
        return arg;
    }, (void*)0x1234);

    fprintf(stderr, "=== Step 5: bthread_create returned %d, tid=%llu ===\n", ret, (unsigned long long)tid);
    fflush(stderr);

    fprintf(stderr, "=== Step 6: Calling bthread_join ===\n");
    fflush(stderr);
    bthread_join(tid, nullptr);

    fprintf(stderr, "=== Step 7: bthread_join returned ===\n");
    fflush(stderr);

    fprintf(stderr, "=== Test PASSED ===\n");
    fflush(stderr);
    return 0;
}