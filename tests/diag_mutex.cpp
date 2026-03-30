/**
 * Diagnostic test for high concurrency mutex hang
 *
 * This test instruments mutex operations to trace the issue.
 */

#ifdef _WIN32
#define _CRT_NONSTDC_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <errno.h>
#endif

#include "bthread.h"
#include "bthread/mutex.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>

// Diagnostic counters
static std::atomic<int> lock_attempts{0};
static std::atomic<int> lock_successes{0};
static std::atomic<int> wait_count{0};
static std::atomic<int> wake_count{0};
static std::atomic<int> wakeup_enqueues{0};

// Track which threads are in wait queue
static std::atomic<int> waiting_threads{0};

static bthread_mutex_t diag_mutex;
static std::atomic<int> counter{0};
static std::atomic<bool> should_stop{false};

// Simpler test with fewer iterations
void* diag_mutex_task(void* arg) {
    int iterations = *static_cast<int*>(arg);
    int id = bthread_self() % 100;  // Simplified ID

    for (int i = 0; i < iterations && !should_stop.load(); ++i) {
        lock_attempts++;

        bthread_mutex_lock(&diag_mutex);
        lock_successes++;
        counter++;
        bthread_mutex_unlock(&diag_mutex);
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    int num_threads = 8;
    int iterations = 100;  // Reduced for quicker testing

    fprintf(stderr, "=== Mutex Diagnostic Test ===\n");
    fprintf(stderr, "Threads: %d, Iterations: %d\n", num_threads, iterations);

    bthread_mutex_init(&diag_mutex, nullptr);
    counter = 0;
    should_stop = false;
    lock_attempts = 0;
    lock_successes = 0;

    std::vector<bthread_t> tids(num_threads);
    std::vector<int> args(num_threads, iterations);

    // Create threads
    for (int i = 0; i < num_threads; ++i) {
        bthread_create(&tids[i], nullptr, diag_mutex_task, &args[i]);
    }

    // Wait with timeout detection
    bool all_done = false;
    for (int wait_round = 0; wait_round < 50 && !all_done; ++wait_round) {
        all_done = true;
        for (int i = 0; i < num_threads; ++i) {
            // Check if thread is done by trying to join with small delay
            // We can't really do timeout join, so just check status periodically
        }

        // Print diagnostic info
        fprintf(stderr, "[Round %d] Lock attempts: %d, Successes: %d, Counter: %d (expected: %d)\n",
                wait_round, lock_attempts.load(), lock_successes.load(),
                counter.load(), num_threads * iterations);

        // Check progress
        if (counter.load() >= num_threads * iterations) {
            break;
        }

        // If no progress for multiple rounds, likely hung
        static int prev_counter = 0;
        if (counter.load() == prev_counter && wait_round > 10) {
            fprintf(stderr, "DEADLOCK DETECTED! No progress after round %d\n", wait_round);
            should_stop = true;
            break;
        }
        prev_counter = counter.load();

        // Small sleep to allow progress
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Try to join all threads
    for (int i = 0; i < num_threads; ++i) {
        bthread_join(tids[i], nullptr);
    }

    fprintf(stderr, "\n=== Results ===\n");
    fprintf(stderr, "Expected: %d, Actual: %d\n", num_threads * iterations, counter.load());
    fprintf(stderr, "Lock attempts: %d, Successes: %d\n", lock_attempts.load(), lock_successes.load());

    if (counter.load() == num_threads * iterations) {
        fprintf(stderr, "TEST PASSED!\n");
    } else {
        fprintf(stderr, "TEST FAILED - incomplete!\n");
    }

    bthread_mutex_destroy(&diag_mutex);

    (void)argc;
    (void)argv;
    return counter.load() == num_threads * iterations ? 0 : 1;
}