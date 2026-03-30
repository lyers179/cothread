#include "bthread.h"
#include "bthread/mutex.h"
#include "bthread/platform/platform.h"

#include <cstdio>
#include <cassert>
#include <thread>

int main() {
    printf("Testing Mutex...\n");

    printf("  Testing init/destroy...\n");
    bthread_mutex_t m;
    int ret = bthread_mutex_init(&m, nullptr);
    assert(ret == 0);

    ret = bthread_mutex_destroy(&m);
    assert(ret == 0);

    printf("  Testing lock/unlock...\n");
    bthread_mutex_t m2;
    bthread_mutex_init(&m2, nullptr);

    ret = bthread_mutex_lock(&m2);
    assert(ret == 0);

    ret = bthread_mutex_unlock(&m2);
    assert(ret == 0);

    printf("  Testing trylock...\n");
    ret = bthread_mutex_lock(&m2);
    assert(ret == 0);

    ret = bthread_mutex_trylock(&m2);
    assert(ret == EBUSY);

    bthread_mutex_unlock(&m2);

    ret = bthread_mutex_trylock(&m2);
    assert(ret == 0);

    bthread_mutex_unlock(&m2);

    printf("  Testing concurrent access...\n");
    bthread_mutex_t m3;
    bthread_mutex_init(&m3, nullptr);

    int counter = 0;
    const int iterations = 1000;

    std::thread t1([&]() {
        for (int i = 0; i < iterations; ++i) {
            bthread_mutex_lock(&m3);
            counter++;
            bthread_mutex_unlock(&m3);
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < iterations; ++i) {
            bthread_mutex_lock(&m3);
            counter++;
            bthread_mutex_unlock(&m3);
        }
    });

    t1.join();
    t2.join();

    assert(counter == iterations * 2);

    bthread_mutex_destroy(&m3);

    printf("All Mutex tests passed!\n");
    return 0;
}