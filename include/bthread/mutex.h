#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
extern "C" {
#endif

struct bthread_mutex_t {
    void* butex;
#ifdef __cplusplus
    std::atomic<uint64_t> owner{0};
#else
    _Atomic uint64_t owner;
#endif
    void* pthread_mutex;
};

int bthread_mutex_init(bthread_mutex_t* mutex, const void* attr);
int bthread_mutex_destroy(bthread_mutex_t* mutex);
int bthread_mutex_lock(bthread_mutex_t* mutex);
int bthread_mutex_unlock(bthread_mutex_t* mutex);
int bthread_mutex_trylock(bthread_mutex_t* mutex);

#ifdef __cplusplus
}
#endif