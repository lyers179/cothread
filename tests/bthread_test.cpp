#include "bthread.h"

#include <cstdio>
#include <cassert>
#include <thread>
#include <atomic>

using namespace bthread;

static int shared_counter = 0;
static bthread_mutex_t mutex;

void* increment_task(void* arg) {
    int iterations = *static_cast<int*>(arg);

    for (int i = 0; i < iterations; ++i) {
        bthread_mutex_lock(&mutex);
        shared_counter++;
        bthread_mutex_unlock(&mutex);
    }

    return arg;
}

void* simple_task(void* arg) {
    int* result = static_cast<int*>(arg);
    *result = 42;
    return result;
}

void* yield_task(void* arg) {
    for (int i = 0; i < 10; ++i) {
        bthread_yield();
    }
    return arg;
}

int main() {
    printf("Testing bthread API...\n");

    printf("  Testing bthread_create/join...\n");
    int result = 0;
    bthread_t tid;
    int ret = bthread_create(&tid, nullptr, simple_task, &result);
    assert(ret == 0);

    void* retval;
    ret = bthread_join(tid, &retval);
    assert(ret == 0);
    assert(retval == &result);
    assert(result == 42);

    printf("  Testing bthread_self...\n");
    bthread_t self_tid = bthread_self();
    // Called from pthread, should return 0
    assert(self_tid == 0);

    printf("  Testing bthread_yield from pthread...\n");
    ret = bthread_yield();
    assert(ret == 0);

    printf("  Testing bthread_detach...\n");
    bthread_t tid2;
    ret = bthread_create(&tid2, nullptr, simple_task, &result);
    assert(ret == 0);

    ret = bthread_detach(tid2);
    assert(ret == 0);

    printf("  Testing multiple bthreads with mutex...\n");
    bthread_mutex_init(&mutex, nullptr);
    shared_counter = 0;

    const int num_threads = 4;
    const int iterations = 1000;
    bthread_t tids[num_threads];
    int thread_args[num_threads];

    for (int i = 0; i < num_threads; ++i) {
        thread_args[i] = iterations;
        ret = bthread_create(&tids[i], nullptr, increment_task, &thread_args[i]);
        assert(ret == 0);
    }

    for (int i = 0; i < num_threads; ++i) {
        bthread_join(tids[i], nullptr);
    }

    assert(shared_counter == num_threads * iterations);
    bthread_mutex_destroy(&mutex);

    printf("  Testing bthread_yield from bthread...\n");
    bthread_t tid3;
    ret = bthread_create(&tid3, nullptr, yield_task, nullptr);
    assert(ret == 0);
    bthread_join(tid3, nullptr);

    printf("  Testing bthread_set_worker_count...\n");
    ret = bthread_set_worker_count(2);
    assert(ret == 0);

    printf("  Testing bthread_get_worker_count...\n");
    int wc = bthread_get_worker_count();
    // Worker count may be already initialized
    assert(wc >= 0);

    printf("All bthread API tests passed!\n");
    return 0;
}