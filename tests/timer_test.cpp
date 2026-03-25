#include "bthread.h"

#include <cstdio>
#include <cassert>

using namespace bthread;

static int timer_called = 0;
static bthread_t timer_tid = 0;

void timer_callback(void* arg) {
    timer_called = 1;
    timer_tid = bthread_self();
    (void)arg;
}

int main() {
    printf("Testing Timer...\n");

    printf("  Testing timer add...\n");
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 100000000;  // 100ms

    bthread_timer_t timer_id = bthread_timer_add(timer_callback, nullptr, &delay);
    assert(timer_id >= 0);

    printf("  Testing timer cancel...\n");
    int ret = bthread_timer_cancel(timer_id);
    assert(ret == 0);

    printf("  Testing invalid timer cancel...\n");
    ret = bthread_timer_cancel(-1);
    assert(ret != 0);

    printf("All Timer tests passed!\n");
    return 0;
}