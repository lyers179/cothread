#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
extern "C" {
#endif

struct bthread_mutex_t {
#ifdef __cplusplus
    std::atomic<void*> butex{nullptr};
    std::atomic<uint64_t> owner{0};
#else
    _Atomic void* butex;
    _Atomic uint64_t owner;
#endif
    void* native_mutex;  // Platform-specific mutex (pthread_mutex_t or SRWLOCK)
};

int bthread_mutex_init(bthread_mutex_t* mutex, const void* attr);
int bthread_mutex_destroy(bthread_mutex_t* mutex);
int bthread_mutex_lock(bthread_mutex_t* mutex);
int bthread_mutex_unlock(bthread_mutex_t* mutex);
int bthread_mutex_trylock(bthread_mutex_t* mutex);

#ifdef __cplusplus
}
#endif