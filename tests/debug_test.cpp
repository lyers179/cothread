#include "bthread.h"
#include "bthread/core/scheduler.hpp"
#include "bthread/core/worker.hpp"
#include "bthread/queue/global_queue.hpp"

#include <cstdio>
#include <thread>
#include <chrono>

void* simple_task(void* arg) {
    int id = (int)(intptr_t)arg;
    printf("    Task %d START\n", id);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    printf("    Task %d END\n", id);
    return arg;
}

int main() {
    printf("=== Debug Wake Test ===\n");

    printf("1. Initializing scheduler...\n");
    bthread::Scheduler::Instance().Init();
    printf("   Worker count: %d\n", bthread::Scheduler::Instance().worker_count());

    printf("2. Creating 2 bthreads...\n");

    bthread_t tid0, tid1;
    bthread_create(&tid0, nullptr, simple_task, (void*)0);
    printf("   Created bthread 0, tid=%llu, global_queue empty=%d\n",
           (unsigned long long)tid0,
           bthread::Scheduler::Instance().global_queue().Empty());

    bthread_create(&tid1, nullptr, simple_task, (void*)1);
    printf("   Created bthread 1, tid=%llu, global_queue empty=%d\n",
           (unsigned long long)tid1,
           bthread::Scheduler::Instance().global_queue().Empty());

    printf("3. Sleeping 100ms to let workers process...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    printf("   After sleep, global_queue empty=%d\n",
           bthread::Scheduler::Instance().global_queue().Empty());

    printf("4. Joining...\n");
    printf("   Joining bthread 0...\n");
    bthread_join(tid0, nullptr);
    printf("   Joined bthread 0\n");

    printf("   Joining bthread 1...\n");
    bthread_join(tid1, nullptr);
    printf("   Joined bthread 1\n");

    printf("=== Test PASSED ===\n");
    return 0;
}