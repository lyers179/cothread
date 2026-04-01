#include "bthread.h"
#include <cstdio>

void* simple_task(void* arg) {
    int id = *static_cast<int*>(arg);
    fprintf(stderr, "Task %d running\n", id);
    return nullptr;
}

int main() {
    setvbuf(stderr, nullptr, _IONBF, 0);

    fprintf(stderr, "Starting minimal test\n");

    for (int i = 0; i < 5; ++i) {
        fprintf(stderr, "Creating bthread %d\n", i);
        bthread_t tid;
        int id = i;
        int ret = bthread_create(&tid, nullptr, simple_task, &id);
        fprintf(stderr, "Created bthread %d, ret=%d\n", i, ret);

        fprintf(stderr, "Joining bthread %d\n", i);
        ret = bthread_join(tid, nullptr);
        fprintf(stderr, "Joined bthread %d, ret=%d\n", i, ret);
    }

    fprintf(stderr, "All done!\n");
    bthread_shutdown();
    fprintf(stderr, "Shutdown complete\n");
    return 0;
}
