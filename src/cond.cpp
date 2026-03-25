#include "bthread/cond.h"
#include "bthread/worker.h"
#include "bthread/butex.h"

#ifdef __cplusplus
#include <pthread.h>

namespace bthread {

} // namespace bthread

extern "C" {

#endif

int bthread_cond_init(bthread_cond_t* cond, const void* attr) {
    (void)attr;
    cond->butex = nullptr;
#ifdef __cplusplus
    cond->pthread_cond = new pthread_cond_t();
    return pthread_cond_init(static_cast<pthread_cond_t*>(cond->pthread_cond), nullptr);
#else
    cond->pthread_cond = nullptr;
    return 0;
#endif
}

int bthread_cond_destroy(bthread_cond_t* cond) {
#ifdef __cplusplus
    if (cond->pthread_cond) {
        pthread_cond_destroy(static_cast<pthread_cond_t*>(cond->pthread_cond));
        delete static_cast<pthread_cond_t*>(cond->pthread_cond);
    }
    if (cond->butex) {
        delete static_cast<Butex*>(cond->butex);
    }
#else
    (void)cond;
#endif
    return 0;
}

int bthread_cond_wait(bthread_cond_t* cond, bthread_mutex_t* mutex) {
#ifdef __cplusplus
    bthread::Worker* w = bthread::Worker::Current();

    if (w) {
        // Called from bthread - use butex
        if (!cond->butex) {
            cond->butex = new Butex();
        }

        // Release mutex and wait
        bthread_mutex_unlock(mutex);
        static_cast<Butex*>(cond->butex)->Wait(0, nullptr);
        bthread_mutex_lock(mutex);

        return 0;
    } else {
        // Called from pthread - use pthread cond
        return pthread_cond_wait(static_cast<pthread_cond_t*>(cond->pthread_cond),
                                static_cast<pthread_mutex_t*>(mutex->pthread_mutex));
    }
#else
    (void)cond;
    (void)mutex;
    return ENOTSUP;
#endif
}

int bthread_cond_timedwait(bthread_cond_t* cond, bthread_mutex_t* mutex,
                           const struct timespec* abstime) {
#ifdef __cplusplus
    bthread::Worker* w = bthread::Worker::Current();

    if (w) {
        // Called from bthread - use butex with timeout
        if (!cond->butex) {
            cond->butex = new Butex();
        }

        // Release mutex and wait
        bthread_mutex_unlock(mutex);
        int ret = static_cast<Butex*>(cond->butex)->Wait(0, abstime);
        bthread_mutex_lock(mutex);

        return ret;
    } else {
        // Called from pthread - use pthread cond
        return pthread_cond_timedwait(static_cast<pthread_cond_t*>(cond->pthread_cond),
                                      static_cast<pthread_mutex_t*>(mutex->pthread_mutex),
                                      abstime);
    }
#else
    (void)cond;
    (void)mutex;
    (void)abstime;
    return ENOTSUP;
#endif
}

int bthread_cond_signal(bthread_cond_t* cond) {
#ifdef __cplusplus
    bthread::Worker* w = bthread::Worker::Current();

    if (w && cond->butex) {
        // Called from bthread - wake one waiter
        static_cast<Butex*>(cond->butex)->Wake(1);
        return 0;
    } else if (cond->pthread_cond) {
        // Called from pthread or no butex - use pthread cond
        return pthread_cond_signal(static_cast<pthread_cond_t*>(cond->pthread_cond));
    }
    return 0;
#else
    (void)cond;
    return 0;
#endif
}

int bthread_cond_broadcast(bthread_cond_t* cond) {
#ifdef __cplusplus
    bthread::Worker* w = bthread::Worker::Current();

    if (w && cond->butex) {
        // Called from bthread - wake all waiters
        static_cast<Butex*>(cond->butex)->Wake(INT_MAX);
        return 0;
    } else if (cond->pthread_cond) {
        // Called from pthread or no butex - use pthread cond
        return pthread_cond_broadcast(static_cast<pthread_cond_t*>(cond->pthread_cond));
    }
    return 0;
#else
    (void)cond;
    return 0;
#endif
}

#ifdef __cplusplus
}
#endif