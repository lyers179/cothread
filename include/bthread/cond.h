#pragma once

#include "bthread/platform/platform.h"
#include "bthread/mutex.h"

#ifdef __cplusplus
extern "C" {
#endif

struct bthread_cond_t {
    void* butex;
    void* native_cond;  // Platform-specific condition variable
};

int bthread_cond_init(bthread_cond_t* cond, const void* attr);
int bthread_cond_destroy(bthread_cond_t* cond);
int bthread_cond_wait(bthread_cond_t* cond, bthread_mutex_t* mutex);
int bthread_cond_timedwait(bthread_cond_t* cond, bthread_mutex_t* mutex,
                           const struct timespec* abstime);
int bthread_cond_signal(bthread_cond_t* cond);
int bthread_cond_broadcast(bthread_cond_t* cond);

#ifdef __cplusplus
}
#endif