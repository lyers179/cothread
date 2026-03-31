#include "bthread.h"

#include <cstdio>
#include <cassert>

static int timer_called = 0;
static bthread_t timer_tid = 0;

void timer_callback(void* arg) {
    timer_called = 1;
    timer_tid = bthread_self();
    (void)arg;
}

int main() {
    // Disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "Testing Timer...\n");

    fprintf(stderr, "  Testing timer add...\n");
    fflush(stderr);
    struct bthread_timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 100000000;  // 100ms

    bthread_timer_t timer_id = bthread_timer_add(timer_callback, nullptr, &delay);
    fprintf(stderr, "  timer_id = %d\n", timer_id);
    fflush(stderr);
    assert(timer_id >= 0);

    fprintf(stderr, "  Testing timer cancel...\n");
    fflush(stderr);
    int ret = bthread_timer_cancel(timer_id);
    fprintf(stderr, "  cancel ret = %d\n", ret);
    fflush(stderr);
    assert(ret == 0);

    fprintf(stderr, "  Testing invalid timer cancel...\n");
    fflush(stderr);
    ret = bthread_timer_cancel(-1);
    assert(ret != 0);

    fprintf(stderr, "All Timer tests passed!\n");
    fflush(stderr);

    bthread_shutdown();  // Cleanup
    return 0;
}