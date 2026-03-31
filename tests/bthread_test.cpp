#include "bthread.h"
#include "bthread/mutex.h"

#include <cstdio>
#include <cassert>
#include <thread>
#include <atomic>

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
    fprintf(stderr, "    yield_task started\n");
    fflush(stderr);
    for (int i = 0; i < 10; ++i) {
        fprintf(stderr, "    yield_task iteration %d\n", i);
        fflush(stderr);
        bthread_yield();
    }
    fprintf(stderr, "    yield_task finished\n");
    fflush(stderr);
    return arg;
}

int main() {
    // Disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "Testing bthread API...\n");

    fprintf(stderr, "  Testing bthread_create/join...\n");
    fflush(stderr);
    int result = 0;
    bthread_t tid;
    int ret = bthread_create(&tid, nullptr, simple_task, &result);
    assert(ret == 0);

    void* retval;
    ret = bthread_join(tid, &retval);
    assert(ret == 0);
    assert(retval == &result);
    assert(result == 42);

    fprintf(stderr, "  Testing bthread_self...\n");
    fflush(stderr);
    bthread_t self_tid = bthread_self();
    // Called from pthread, should return 0
    assert(self_tid == 0);

    fprintf(stderr, "  Testing bthread_yield from pthread...\n");
    fflush(stderr);
    ret = bthread_yield();
    assert(ret == 0);

    fprintf(stderr, "  Testing bthread_detach...\n");
    fflush(stderr);
    bthread_t tid2;
    ret = bthread_create(&tid2, nullptr, simple_task, &result);
    assert(ret == 0);

    ret = bthread_detach(tid2);
    assert(ret == 0);

    fprintf(stderr, "  Testing multiple bthreads with mutex...\n");
    fflush(stderr);
    bthread_mutex_init(&mutex, nullptr);
    shared_counter = 0;

    const int num_threads = 4;
    const int iterations = 1000;
    bthread_t tids[4];
    int thread_args[4];

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

    fprintf(stderr, "  Testing bthread_yield from bthread...\n");
    fflush(stderr);
    bthread_t tid3;
    ret = bthread_create(&tid3, nullptr, yield_task, nullptr);
    assert(ret == 0);
    bthread_join(tid3, nullptr);

    fprintf(stderr, "  Testing bthread_set_worker_count...\n");
    fflush(stderr);
    ret = bthread_set_worker_count(2);
    // May return EBUSY if already initialized, which is expected
    assert(ret == 0 || ret == EBUSY);

    fprintf(stderr, "  Testing bthread_get_worker_count...\n");
    fflush(stderr);
    int wc = bthread_get_worker_count();
    // Worker count may be already initialized
    assert(wc >= 0);

    fprintf(stderr, "All bthread API tests passed!\n");
    fflush(stderr);
    return 0;
}