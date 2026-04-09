// Performance profiling target - runs for extended time
#include "bthread.h"
#include "bthread/sync/mutex.hpp"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <thread>
#include <atomic>

void* simple_task(void* arg) {
    (void)arg;
    return nullptr;
}

void* yield_task(void* arg) {
    int count = *static_cast<int*>(arg);
    for (int i = 0; i < count; ++i) {
        bthread_yield();
    }
    return nullptr;
}

void* mutex_task(void* arg) {
    auto* mutex = static_cast<bthread::Mutex*>(arg);
    for (int i = 0; i < 100; ++i) {
        mutex->lock();
        mutex->unlock();
    }
    return nullptr;
}

int main(int argc, char** argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    int duration_sec = argc > 2 ? atoi(argv[2]) : 10;

    bthread_set_worker_count(8);

    printf("Running perf profiling target, mode=%d, duration=%ds\n", mode, duration_sec);

    auto start = std::chrono::high_resolution_clock::now();
    std::atomic<long> total_ops{0};

    while (std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::high_resolution_clock::now() - start).count() < duration_sec) {

        switch (mode) {
            case 0: { // Create/Join benchmark
                constexpr int BATCH = 50;
                std::vector<bthread_t> tids(BATCH);
                for (int i = 0; i < BATCH; ++i) {
                    bthread_create(&tids[i], nullptr, simple_task, nullptr);
                }
                for (int i = 0; i < BATCH; ++i) {
                    bthread_join(tids[i], nullptr);
                }
                total_ops += BATCH;
                break;
            }
            case 1: { // Yield benchmark
                bthread_t tid;
                int yields = 100;
                bthread_create(&tid, nullptr, yield_task, &yields);
                bthread_join(tid, nullptr);
                total_ops += 100;
                break;
            }
            case 2: { // Mutex benchmark
                static bthread::Mutex mutex;
                constexpr int NUM = 10;
                std::vector<bthread_t> tids(NUM);
                for (int i = 0; i < NUM; ++i) {
                    bthread_create(&tids[i], nullptr, mutex_task, &mutex);
                }
                for (int i = 0; i < NUM; ++i) {
                    bthread_join(tids[i], nullptr);
                }
                total_ops += NUM * 100;
                break;
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    printf("Total ops: %ld, Throughput: %.0f ops/sec\n",
           total_ops.load(), total_ops.load() / elapsed);

    // Don't call shutdown - let the process exit naturally
    // bthread_shutdown();
    return 0;
}