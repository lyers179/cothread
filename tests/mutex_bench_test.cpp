#include "bthread.h"
#include <cstdio>
#include <atomic>

static std::atomic<int> counter{0};

void* simple_task(void* arg) {
    counter.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

int main() {
    setvbuf(stderr, nullptr, _IONBF, 0);
    const int iterations = 20;
    fprintf(stderr, "Test: Rapid create/join (%d iterations)\n", iterations);
    counter = 0;
    for (int i = 0; i < iterations; ++i) {
        bthread_t tid;
        bthread_create(&tid, nullptr, simple_task, nullptr);
        bthread_join(tid, nullptr);
    }
    fprintf(stderr, "Counter: %d\n", counter.load());
    bthread_shutdown();
    fprintf(stderr, "Shutdown complete\n");
    return 0;
}
