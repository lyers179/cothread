#include "bthread/cond.h"
#include "bthread/mutex.h"
#include "bthread/worker.h"
#include "bthread/butex.h"
#include "bthread/platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace bthread {

} // namespace bthread

extern "C" {

int bthread_cond_init(bthread_cond_t* cond, const void* attr) {
    (void)attr;
    cond->butex = nullptr;
#ifdef _WIN32
    cond->native_cond = new CONDITION_VARIABLE();
    InitializeConditionVariable(static_cast<CONDITION_VARIABLE*>(cond->native_cond));
#else
    cond->native_cond = new pthread_cond_t();
    pthread_cond_init(static_cast<pthread_cond_t*>(cond->native_cond), nullptr);
#endif
    return 0;
}

int bthread_cond_destroy(bthread_cond_t* cond) {
    if (cond->butex) {
        delete static_cast<bthread::Butex*>(cond->butex);
    }
    if (cond->native_cond) {
#ifdef _WIN32
        // CONDITION_VARIABLE doesn't need explicit destruction
        delete static_cast<CONDITION_VARIABLE*>(cond->native_cond);
#else
        pthread_cond_destroy(static_cast<pthread_cond_t*>(cond->native_cond));
        delete static_cast<pthread_cond_t*>(cond->native_cond);
#endif
    }
    return 0;
}

int bthread_cond_wait(bthread_cond_t* cond, bthread_mutex_t* mutex) {
    bthread::Worker* w = bthread::Worker::Current();

    if (w) {
        // Called from bthread - use butex with generation mechanism
        if (!cond->butex) {
            cond->butex = new bthread::Butex();
        }

        bthread::Butex* butex = static_cast<bthread::Butex*>(cond->butex);

        // Capture current generation before releasing mutex
        // This prevents missing signals that occur after unlock
        int generation = butex->value();

        // Release mutex and wait for generation change
        bthread_mutex_unlock(mutex);
        butex->Wait(generation, nullptr);
        bthread_mutex_lock(mutex);

        return 0;
    } else {
        // Called from pthread - use native cond
#ifdef _WIN32
        return SleepConditionVariableSRW(static_cast<CONDITION_VARIABLE*>(cond->native_cond),
                                         static_cast<SRWLOCK*>(mutex->native_mutex),
                                         INFINITE, 0) ? 0 : bthread::platform::EINVAL;
#else
        return pthread_cond_wait(static_cast<pthread_cond_t*>(cond->native_cond),
                                static_cast<pthread_mutex_t*>(mutex->native_mutex));
#endif
    }
}

int bthread_cond_timedwait(bthread_cond_t* cond, bthread_mutex_t* mutex,
                           const struct timespec* abstime) {
    bthread::Worker* w = bthread::Worker::Current();

    if (w) {
        // Called from bthread - use butex with generation mechanism
        if (!cond->butex) {
            cond->butex = new bthread::Butex();
        }

        bthread::Butex* butex = static_cast<bthread::Butex*>(cond->butex);

        // Capture current generation before releasing mutex
        int generation = butex->value();

        // Convert to platform timespec
        bthread::platform::timespec platform_ts;
        if (abstime) {
            platform_ts.tv_sec = abstime->tv_sec;
            platform_ts.tv_nsec = abstime->tv_nsec;
        }

        // Release mutex and wait for generation change
        bthread_mutex_unlock(mutex);
        int ret = butex->Wait(generation, abstime ? &platform_ts : nullptr);
        bthread_mutex_lock(mutex);

        return ret;
    } else {
        // Called from pthread - use native cond
#ifdef _WIN32
        // Convert timespec to milliseconds
        DWORD ms = INFINITE;
        if (abstime) {
            int64_t now_us = bthread::platform::GetTimeOfDayUs();
            int64_t target_us = abstime->tv_sec * 1000000 + abstime->tv_nsec / 1000;
            int64_t diff_ms = (target_us - now_us) / 1000;
            if (diff_ms <= 0) {
                return bthread::platform::ETIMEDOUT;
            }
            ms = static_cast<DWORD>(diff_ms);
        }
        BOOL result = SleepConditionVariableSRW(static_cast<CONDITION_VARIABLE*>(cond->native_cond),
                                                 static_cast<SRWLOCK*>(mutex->native_mutex),
                                                 ms, 0);
        return result ? 0 : (GetLastError() == ERROR_TIMEOUT ? bthread::platform::ETIMEDOUT : bthread::platform::EINVAL);
#else
        return pthread_cond_timedwait(static_cast<pthread_cond_t*>(cond->native_cond),
                                      static_cast<pthread_mutex_t*>(mutex->native_mutex),
                                      abstime);
#endif
    }
}

int bthread_cond_signal(bthread_cond_t* cond) {
    bthread::Worker* w = bthread::Worker::Current();

    if (w && cond->butex) {
        // Called from bthread - increment generation and wake one waiter
        bthread::Butex* butex = static_cast<bthread::Butex*>(cond->butex);
        butex->set_value(butex->value() + 1);
        butex->Wake(1);
        return 0;
    } else if (cond->native_cond) {
        // Called from pthread or no butex - use native cond
#ifdef _WIN32
        WakeConditionVariable(static_cast<CONDITION_VARIABLE*>(cond->native_cond));
        return 0;
#else
        return pthread_cond_signal(static_cast<pthread_cond_t*>(cond->native_cond));
#endif
    }
    return 0;
}

int bthread_cond_broadcast(bthread_cond_t* cond) {
    bthread::Worker* w = bthread::Worker::Current();

    if (w && cond->butex) {
        // Called from bthread - increment generation and wake all waiters
        bthread::Butex* butex = static_cast<bthread::Butex*>(cond->butex);
        butex->set_value(butex->value() + 1);
        butex->Wake(INT_MAX);
        return 0;
    } else if (cond->native_cond) {
        // Called from pthread or no butex - use native cond
#ifdef _WIN32
        WakeAllConditionVariable(static_cast<CONDITION_VARIABLE*>(cond->native_cond));
        return 0;
#else
        return pthread_cond_broadcast(static_cast<pthread_cond_t*>(cond->native_cond));
#endif
    }
    return 0;
}

}