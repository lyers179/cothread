#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// ========== Basic bthread API ==========
typedef uint64_t bthread_t;

// Thread attributes
#define BTHREAD_STACK_SIZE_DEFAULT (1024 * 1024)

typedef struct {
    size_t stack_size;
    const char* name;
} bthread_attr_t;

#define BTHREAD_ATTR_INIT { BTHREAD_STACK_SIZE_DEFAULT, NULL }

static inline int bthread_attr_init(bthread_attr_t* attr) {
    attr->stack_size = BTHREAD_STACK_SIZE_DEFAULT;
    attr->name = NULL;
    return 0;
}

static inline int bthread_attr_destroy(bthread_attr_t* attr) {
    (void)attr;
    return 0;
}

// Create a new bthread
int bthread_create(bthread_t* tid, const bthread_attr_t* attr,
                   void* (*fn)(void*), void* arg);

// Wait for bthread to complete
int bthread_join(bthread_t tid, void** retval);

// Detach bthread (auto-clean on exit)
int bthread_detach(bthread_t tid);

// Get current bthread ID
bthread_t bthread_self(void);

// Yield current bthread
int bthread_yield(void);

// Exit current bthread
void bthread_exit(void* retval);

// ========== Synchronization Primitives ==========
typedef struct bthread_mutex_t bthread_mutex_t;
typedef struct bthread_cond_t bthread_cond_t;
typedef struct bthread_once_t bthread_once_t;

// Mutex
int bthread_mutex_init(bthread_mutex_t* mutex, const void* attr);
int bthread_mutex_destroy(bthread_mutex_t* mutex);
int bthread_mutex_lock(bthread_mutex_t* mutex);
int bthread_mutex_unlock(bthread_mutex_t* mutex);
int bthread_mutex_trylock(bthread_mutex_t* mutex);

// Condition Variable
int bthread_cond_init(bthread_cond_t* cond, const void* attr);
int bthread_cond_destroy(bthread_cond_t* cond);
int bthread_cond_wait(bthread_cond_t* cond, bthread_mutex_t* mutex);
int bthread_cond_timedwait(bthread_cond_t* cond, bthread_mutex_t* mutex,
                           const struct timespec* abstime);
int bthread_cond_signal(bthread_cond_t* cond);
int bthread_cond_broadcast(bthread_cond_t* cond);

// One-time initialization
#define BTHREAD_ONCE_INIT {0}

typedef struct {
    int state;
    void* once_ptr;
} bthread_once_t;

int bthread_once(bthread_once_t* once, void (*init_routine)(void));

// ========== Timer ==========
typedef int bthread_timer_t;

bthread_timer_t bthread_timer_add(void (*callback)(void*), void* arg,
                                   const struct timespec* delay);
int bthread_timer_cancel(bthread_timer_t timer_id);

// ========== Global Configuration ==========
int bthread_set_worker_count(int count);
int bthread_get_worker_count(void);

// Error codes (use errno values)
#define BTHREAD_SUCCESS 0
#define BTHREAD_EINVAL 22
#define BTHREAD_ENOMEM 12
#define BTHREAD_EAGAIN 11
#define BTHREAD_ESRCH 3
#define BTHREAD_ETIMEDOUT 110
#define BTHREAD_EDEADLK 35
#define BTHREAD_EBUSY 16

#ifdef __cplusplus
}
#endif