#include "bthread/mutex.h"
#include "bthread/butex.h"
#include "bthread/worker.h"
#include "bthread/scheduler.h"
#include "bthread/platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <cstring>

namespace bthread {

// Simple 2-part mutex (busy + butex)
constexpr uint64_t LOCKED = 1;
constexpr uint64_t HAS_WAITERS = 2;

void bthread_mutex_t_init_impl(bthread_mutex_t* mutex) {
    mutex->butex.store(nullptr, std::memory_order_relaxed);
    mutex->owner.store(0, std::memory_order_relaxed);
#ifdef _WIN32
    mutex->native_mutex = new SRWLOCK();
    InitializeSRWLock(static_cast<SRWLOCK*>(mutex->native_mutex));
#else
    mutex->native_mutex = new pthread_mutex_t();
    pthread_mutex_init(static_cast<pthread_mutex_t*>(mutex->native_mutex), nullptr);
#endif
}

void bthread_mutex_t_destroy_impl(bthread_mutex_t* mutex) {
    void* butex_ptr = mutex->butex.load(std::memory_order_acquire);
    delete static_cast<Butex*>(butex_ptr);
    if (mutex->native_mutex) {
#ifdef _WIN32
        // SRWLOCK doesn't need explicit destruction
        delete static_cast<SRWLOCK*>(mutex->native_mutex);
#else
        pthread_mutex_destroy(static_cast<pthread_mutex_t*>(mutex->native_mutex));
        delete static_cast<pthread_mutex_t*>(mutex->native_mutex);
#endif
    }
}

} // namespace bthread

extern "C" {

int bthread_mutex_init(bthread_mutex_t* mutex, const void* attr) {
    (void)attr;
    bthread::bthread_mutex_t_init_impl(mutex);
    return 0;
}

int bthread_mutex_destroy(bthread_mutex_t* mutex) {
    bthread::bthread_mutex_t_destroy_impl(mutex);
    return 0;
}

int bthread_mutex_lock(bthread_mutex_t* mutex) {
    bthread::Worker* w = bthread::Worker::Current();

    if (w) {
        // Called from bthread
        uint64_t expected = 0;
        if (mutex->owner.compare_exchange_strong(expected, bthread::LOCKED,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return 0;
        }

        // Create butex if needed
        if (!mutex->butex.load(std::memory_order_acquire)) {
            bthread::Butex* new_butex = new bthread::Butex();
            void* expected_butex = nullptr;
            if (mutex->butex.compare_exchange_strong(expected_butex, new_butex,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // Successfully installed butex
            } else {
                // Someone else installed a butex
                delete new_butex;
            }
        }

        while (true) {
            // Try to acquire the lock
            // Owner can be: 0 (free), LOCKED (held), LOCKED|HAS_WAITERS (held with waiters), HAS_WAITERS (free but has waiters)
            expected = mutex->owner.load(std::memory_order_acquire);

            if ((expected & bthread::LOCKED) == 0) {
                // Lock appears free, try to acquire
                uint64_t new_val = bthread::LOCKED | (expected & bthread::HAS_WAITERS);
                if (mutex->owner.compare_exchange_strong(expected, new_val,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    return 0;
                }
                // CAS failed, retry
                continue;
            }

            // Lock is held, mark that we're waiting and sleep
            if ((expected & bthread::HAS_WAITERS) == 0) {
                // Set HAS_WAITERS flag
                if (!mutex->owner.compare_exchange_strong(expected, expected | bthread::HAS_WAITERS,
                        std::memory_order_release, std::memory_order_relaxed)) {
                    continue;  // State changed, retry
                }
            }

            // Capture current generation before waiting
            bthread::Butex* butex = static_cast<bthread::Butex*>(
                mutex->butex.load(std::memory_order_acquire));
            int generation = butex->value();

            // Wait for unlock (generation change)
            butex->Wait(generation, nullptr);
        }
    } else {
        // Called from pthread, use native mutex
#ifdef _WIN32
        AcquireSRWLockExclusive(static_cast<SRWLOCK*>(mutex->native_mutex));
        return 0;
#else
        return pthread_mutex_lock(static_cast<pthread_mutex_t*>(mutex->native_mutex));
#endif
    }
}

int bthread_mutex_unlock(bthread_mutex_t* mutex) {
    bthread::Worker* w = bthread::Worker::Current();

    if (w) {
        // Called from bthread
        uint64_t old_owner = mutex->owner.fetch_and(~bthread::LOCKED, std::memory_order_release);
        if (old_owner & bthread::HAS_WAITERS) {
            // Wake one waiter - increment generation first
            bthread::Butex* butex = static_cast<bthread::Butex*>(
                mutex->butex.load(std::memory_order_acquire));
            if (butex) {
                butex->set_value(butex->value() + 1);
                butex->Wake(1);
            }
        }
        return 0;
    } else {
        // Called from pthread, use native mutex
#ifdef _WIN32
        ReleaseSRWLockExclusive(static_cast<SRWLOCK*>(mutex->native_mutex));
        return 0;
#else
        return pthread_mutex_unlock(static_cast<pthread_mutex_t*>(mutex->native_mutex));
#endif
    }
}

int bthread_mutex_trylock(bthread_mutex_t* mutex) {
    bthread::Worker* w = bthread::Worker::Current();

    if (w) {
        // Called from bthread
        uint64_t expected = 0;
        if (mutex->owner.compare_exchange_strong(expected, bthread::LOCKED,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return 0;
        }
        return bthread::platform::EBUSY_VAL;
    } else {
        // Called from pthread, use native mutex
#ifdef _WIN32
        return TryAcquireSRWLockExclusive(static_cast<SRWLOCK*>(mutex->native_mutex)) ? 0 : bthread::platform::EBUSY_VAL;
#else
        return pthread_mutex_trylock(static_cast<pthread_mutex_t*>(mutex->native_mutex));
#endif
    }
}

}