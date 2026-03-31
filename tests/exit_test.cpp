#include "bthread.h"
#include <cstdio>
#include <cstdlib>

void* hello_task(void* arg) {
    printf("Hello from bthread!\n");
    return arg;
}

void cleanup() {
    printf("atexit: cleanup called\n");
}

int main() {
    atexit(cleanup);
    printf("=== Exit Test ===\n");

    bthread_t tid;
    bthread_create(&tid, nullptr, hello_task, nullptr);
    bthread_join(tid, nullptr);
    printf("Joined\n");

    printf("Shutting down...\n");
    bthread_shutdown();
    printf("Shutdown complete\n");

    printf("Returning from main\n");
    return 0;
}
