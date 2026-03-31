#include "bthread.h"
#include <cstdio>

void* hello_task(void* arg) {
    printf("Hello from bthread!\n");
    return arg;
}

int main() {
    printf("=== Simple Test ===\n");

    bthread_t tid;
    bthread_create(&tid, nullptr, hello_task, nullptr);
    bthread_join(tid, nullptr);
    printf("Joined\n");

    printf("Shutting down...\n");
    bthread_shutdown();
    printf("Shutdown complete\n");

    return 0;
}
