#include "bthread/mutex.h"
#include "bthread/butex.h"
#include "bthread/worker.h"
#include "bthread/scheduler.h"
#include "bthread/platform/platform.h"

#include <cstring>

#ifdef __cplusplus
#include <pthread.h>
namespace {
#else
#include <stdatomic.h>
#endif

// Simple 2-part mutex (busy + butex)
constexpr uint64_t LOCKED = 1;
constexpr uint64_t HAS_WAITERS = 2;

#ifdef __cplusplus
} // namespace

namespace bthread {

#endif

#ifdef __cplusplus
void bthread_mutex_t_init_impl(bthread_mutex_t* mutex) {
    mutex->butex = nullptr;
    mutex->owner.store(0, std::memory_order_relaxed);
    mutex->pthread_mutex = new pthread_mutex_t();
    pthread_mutex_init(static_cast<pthread_mutex_t*>(mutex->pthread_mutex), nullptr);
}

void bthread_mutex_t_destroy_impl(bthread_mutex_t* mutex) {
    delete static_cast<Butex*>(mutex->butex);
    if (mutex->pthread_mutex) {
        pthread_mutex_destroy(static_cast<pthread_mutex_t*>(mutex->pthread_mutex));
        delete static_cast<pthread_mutex_t*>(mutex->pthread_mutex);
    }
}

} // namespace bthread

extern "C" {
#else
void bthread_mutex_t_init_impl(bthread_mutex_t* mutex) {
    mutex->butex = NULL;
    atomic_store(&mutex->owner, 0);
}

void bthread_mutex_t_destroy_impl(bthread_mutex_t* mutex) {
    // Butex and pthread_mutex would be freed in C++ implementation
    (void)mutex;
}
#endif

int bthread_mutex_init(bthread_mutex_t* mutex, const void* attr) {
    (void)attr;
    bthread_mutex_t_init_impl(mutex);
    return 0;
}

int bthread_mutex_destroy(bthread_mutex_t* mutex) {
    bthread_mutex_t_destroy_impl(mutex);
    return 0;
}

int bthread_mutex_lock(bthread_mutex_t* mutex) {
#ifdef __cplusplus
    Worker* w = Worker::Current();

    if (w) {
        // Called from bthread
        uint64_t expected = 0;
        if (mutex->owner.compare_exchange_strong(expected, LOCKED,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return 0;
        }

        // Create butex if needed
        if (!mutex->butex) {
            Butex* new_butex = new Butex();
            Butex* expected_butex = nullptr;
            if (mutex->butex.compare_exchange_strong(expected_butex, new_butex,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // Successfully installed butex
            } else {
                // Someone else installed a butex
                delete new_butex;
            }
        }

        while (true) {
            expected = LOCKED;
            if (mutex->owner.compare_exchange_strong(expected, LOCKED,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return 0;
            }

            // Mark as having waiters and wait
            if (expected == LOCKED) {
                mutex->owner.fetch_or(HAS_WAITERS, std::memory_order_release);
                static_cast<Butex*>(mutex->butex)->Wait(0, nullptr);
            }
        }
    } else {
        // Called from pthread, use pthread mutex
        return pthread_mutex_lock(static_cast<pthread_mutex_t*>(mutex->pthread_mutex));
    }
#else
    // C implementation would use platform futex
    (void)mutex;
    return ENOTSUP;
#endif
}

int bthread_mutex_unlock(bthread_mutex_t* mutex) {
#ifdef __cplusplus
    Worker* w = Worker::Current();

    if (w) {
        // Called from bthread
        uint64_t old_owner = mutex->owner.fetch_and(~LOCKED, std::memory_order_release);
        if (old_owner & HAS_WAITERS) {
            // Wake one waiter
            static_cast<Butex*>(mutex->butex)->Wake(1);
        }
        return 0;
    } else {
        // Called from pthread, use pthread mutex
        return pthread_mutex_unlock(static_cast<pthread_mutex_t*>(mutex->pthread_mutex));
    }
#else
    (void)mutex;
    return ENOTSUP;
#endif
}

int bthread_mutex_trylock(bthread_mutex_t* mutex) {
#ifdef __cplusplus
    Worker* w = Worker::Current();

    if (w) {
        // Called from bthread
        uint64_t expected = 0;
        if (mutex->owner.compare_exchange_strong(expected, LOCKED,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return 0;
        }
        return EBUSY;
    } else {
        // Called from pthread, use pthread mutex
        return pthread_mutex_trylock(static_cast<pthread_mutex_t*>(mutex->pthread_mutex));
    }
#else
    (void)mutex;
    return ENOTSUP;
#endif
}