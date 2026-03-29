/**
 * bthread Library Demo
 *
 * Features demonstrated:
 * 1. Create and join bthread
 * 2. Mutex protecting shared data
 * 3. Condition variable synchronization
 * 4. bthread_yield
 * 5. Timer functionality
 */

#include "bthread.h"
#include "bthread/mutex.h"
#include "bthread/cond.h"

#include <cstdio>
#include <cstdlib>

// ==================== Demo 1: Basic bthread create/join ====================

void* hello_task(void* arg) {
    const char* name = static_cast<const char*>(arg);
    fprintf(stderr, "[Demo 1] Hello from %s!\n", name);
    return nullptr;
}

void demo_basic() {
    fprintf(stderr, "\n=== Demo 1: Basic bthread create/join ===\n");

    bthread_t tid1, tid2;

    bthread_create(&tid1, nullptr, hello_task, (void*)"Thread-1");
    bthread_create(&tid2, nullptr, hello_task, (void*)"Thread-2");

    bthread_join(tid1, nullptr);
    bthread_join(tid2, nullptr);

    fprintf(stderr, "[Demo 1] Done\n");
}

// ==================== Demo 2: Mutex protecting shared data ====================

static int shared_counter = 0;
static bthread_mutex_t counter_mutex;

void* increment_task(void* arg) {
    int iterations = *static_cast<int*>(arg);

    for (int i = 0; i < iterations; ++i) {
        bthread_mutex_lock(&counter_mutex);
        shared_counter++;
        bthread_mutex_unlock(&counter_mutex);
    }

    return nullptr;
}

void demo_mutex() {
    fprintf(stderr, "\n=== Demo 2: Mutex protecting shared data ===\n");

    bthread_mutex_init(&counter_mutex, nullptr);
    shared_counter = 0;

    const int num_threads = 4;
    const int iterations = 1000;

    bthread_t tids[4];
    int args[4];

    for (int i = 0; i < num_threads; ++i) {
        args[i] = iterations;
        bthread_create(&tids[i], nullptr, increment_task, &args[i]);
    }

    for (int i = 0; i < num_threads; ++i) {
        bthread_join(tids[i], nullptr);
    }

    fprintf(stderr, "[Demo 2] Final counter: %d (expected: %d)\n",
            shared_counter, num_threads * iterations);

    bthread_mutex_destroy(&counter_mutex);
    fprintf(stderr, "[Demo 2] Done\n");
}

// ==================== Demo 3: Condition variable ====================

static bthread_mutex_t cond_mutex;
static bthread_cond_t cond_var;
static bool ready = false;

void* waiter_task(void* arg) {
    int id = *static_cast<int*>(arg);

    bthread_mutex_lock(&cond_mutex);
    while (!ready) {
        bthread_cond_wait(&cond_var, &cond_mutex);
    }
    bthread_mutex_unlock(&cond_mutex);

    fprintf(stderr, "[Demo 3] Waiter %d woke up\n", id);
    return nullptr;
}

void* signaler_task(void* arg) {
    (void)arg;

    fprintf(stderr, "[Demo 3] Signaler preparing...\n");

    // Simulate some work
    for (int i = 0; i < 3; ++i) {
        bthread_yield();
    }

    bthread_mutex_lock(&cond_mutex);
    ready = true;
    bthread_cond_broadcast(&cond_var);
    bthread_mutex_unlock(&cond_mutex);

    fprintf(stderr, "[Demo 3] Broadcast signal sent\n");
    return nullptr;
}

void demo_condition() {
    fprintf(stderr, "\n=== Demo 3: Condition variable ===\n");

    bthread_mutex_init(&cond_mutex, nullptr);
    bthread_cond_init(&cond_var, nullptr);
    ready = false;

    bthread_t waiters[3];
    int ids[3] = {1, 2, 3};

    for (int i = 0; i < 3; ++i) {
        bthread_create(&waiters[i], nullptr, waiter_task, &ids[i]);
    }

    bthread_t signaler;
    bthread_create(&signaler, nullptr, signaler_task, nullptr);

    for (int i = 0; i < 3; ++i) {
        bthread_join(waiters[i], nullptr);
    }
    bthread_join(signaler, nullptr);

    bthread_mutex_destroy(&cond_mutex);
    bthread_cond_destroy(&cond_var);
    fprintf(stderr, "[Demo 3] Done\n");
}

// ==================== Demo 4: bthread_yield ====================

void* yield_task(void* arg) {
    int id = *static_cast<int*>(arg);

    for (int i = 0; i < 5; ++i) {
        fprintf(stderr, "[Demo 4] Task %d iteration %d\n", id, i);
        bthread_yield();
    }

    return nullptr;
}

void demo_yield() {
    fprintf(stderr, "\n=== Demo 4: bthread_yield ===\n");

    bthread_t tids[2];
    int ids[2] = {1, 2};

    for (int i = 0; i < 2; ++i) {
        bthread_create(&tids[i], nullptr, yield_task, &ids[i]);
    }

    for (int i = 0; i < 2; ++i) {
        bthread_join(tids[i], nullptr);
    }

    fprintf(stderr, "[Demo 4] Done\n");
}

// ==================== Demo 5: Timer ====================

static int timer_count = 0;

void timer_callback(void* arg) {
    int id = *static_cast<int*>(arg);
    timer_count++;
    fprintf(stderr, "[Demo 5] Timer %d fired! (total: %d)\n", id, timer_count);
}

void demo_timer() {
    fprintf(stderr, "\n=== Demo 5: Timer ===\n");

    timer_count = 0;
    int ids[3] = {1, 2, 3};

    // Add timers to fire after 100ms, 200ms, 300ms
    for (int i = 0; i < 3; ++i) {
        bthread_timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = (i + 1) * 100000000;  // 100ms * (i+1)

        bthread_timer_t timer = bthread_timer_add(timer_callback, &ids[i], &delay);
        fprintf(stderr, "[Demo 5] Added timer %d, id=%d\n", ids[i], timer);
    }

    // Wait for timers to fire
    fprintf(stderr, "[Demo 5] Waiting for timers...\n");

    // Simple wait (in production, use proper synchronization)
    for (int i = 0; i < 500 && timer_count < 3; ++i) {
        bthread_yield();
    }

    fprintf(stderr, "[Demo 5] Done\n");
}

// ==================== Demo 6: Detached thread ====================

void* detached_task(void* arg) {
    int id = *static_cast<int*>(arg);
    fprintf(stderr, "[Demo 6] Detached thread %d running\n", id);

    for (int i = 0; i < 3; ++i) {
        bthread_yield();
    }

    fprintf(stderr, "[Demo 6] Detached thread %d finished\n", id);
    return nullptr;
}

void demo_detach() {
    fprintf(stderr, "\n=== Demo 6: Detached thread ===\n");

    bthread_t tid;
    int id = 42;

    bthread_create(&tid, nullptr, detached_task, &id);
    bthread_detach(tid);

    // Give detached thread some time to run
    for (int i = 0; i < 5; ++i) {
        bthread_yield();
    }

    fprintf(stderr, "[Demo 6] Done\n");
}

// ==================== Main ====================

int main() {
    // Disable buffering
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    fprintf(stderr, "========================================\n");
    fprintf(stderr, "       bthread Library Demo\n");
    fprintf(stderr, "========================================\n");

    // Run all demos
    demo_basic();
    demo_mutex();
    demo_condition();
    demo_yield();
    demo_timer();
    demo_detach();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "       All demos completed!\n");
    fprintf(stderr, "========================================\n");

    return 0;
}