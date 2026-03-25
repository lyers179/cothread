#include "bthread.h"
#include "bthread/cond.h"

#include <cstdio>
#include <cassert>
#include <thread>

using namespace bthread;

int main() {
    printf("Testing Condition Variable...\n");

    printf("  Testing init/destroy...\n");
    bthread_cond_t c;
    int ret = bthread_cond_init(&c, nullptr);
    assert(ret == 0);

    ret = bthread_cond_destroy(&c);
    assert(ret == 0);

    printf("  Testing signal/broadcast...\n");
    bthread_cond_t c2;
    bthread_cond_init(&c2, nullptr);

    ret = bthread_cond_signal(&c2);
    assert(ret == 0);

    ret = bthread_cond_broadcast(&c2);
    assert(ret == 0);

    bthread_cond_destroy(&c2);

    printf("All Cond tests passed!\n");
    return 0;
}