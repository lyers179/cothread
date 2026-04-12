#include "bthread.h"

#include <cstdio>
#include <cassert>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>

// Test 1: ScheduleAndExecute - Basic timer scheduling and execution
static std::atomic<int> test1_counter{0};

void test1_callback(void* arg) {
    test1_counter.fetch_add(1, std::memory_order_relaxed);
    (void)arg;
}

static bool test_schedule_and_execute() {
    fprintf(stderr, "  Testing basic schedule and execute...\n");
    fflush(stderr);

    test1_counter.store(0, std::memory_order_release);

    // Schedule a timer with 50ms delay
    struct bthread_timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 50000000;  // 50ms

    bthread_timer_t timer_id = bthread_timer_add(test1_callback, nullptr, &delay);
    if (timer_id < 0) {
        fprintf(stderr, "    FAIL: Failed to schedule timer\n");
        return false;
    }

    // Wait for timer to execute (wait 200ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (test1_counter.load(std::memory_order_acquire) != 1) {
        fprintf(stderr, "    FAIL: Timer not executed, counter=%d\n",
                test1_counter.load(std::memory_order_relaxed));
        return false;
    }

    fprintf(stderr, "    PASS: Timer executed correctly\n");
    return true;
}

// Test 2: MultipleTimers - Schedule multiple timers
static std::atomic<int> test2_counter{0};

void test2_callback(void* arg) {
    int id = reinterpret_cast<intptr_t>(arg);
    test2_counter.fetch_add(id, std::memory_order_relaxed);
}

static bool test_multiple_timers() {
    fprintf(stderr, "  Testing multiple timers...\n");
    fflush(stderr);

    test2_counter.store(0, std::memory_order_release);

    // Schedule 10 timers with 30ms delay each
    struct bthread_timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 30000000;  // 30ms

    std::vector<bthread_timer_t> timer_ids;
    for (int i = 1; i <= 10; ++i) {
        bthread_timer_t timer_id = bthread_timer_add(
            test2_callback,
            reinterpret_cast<void*>(static_cast<intptr_t>(i)),
            &delay);
        if (timer_id < 0) {
            fprintf(stderr, "    FAIL: Failed to schedule timer %d\n", i);
            return false;
        }
        timer_ids.push_back(timer_id);
    }

    // Wait for all timers to execute (wait 200ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Expected sum: 1+2+3+...+10 = 55
    int expected_sum = 55;
    int actual = test2_counter.load(std::memory_order_acquire);
    if (actual != expected_sum) {
        fprintf(stderr, "    FAIL: Counter mismatch, expected=%d, actual=%d\n",
                expected_sum, actual);
        return false;
    }

    fprintf(stderr, "    PASS: All 10 timers executed correctly (sum=%d)\n", actual);
    return true;
}

// Test 3: CancelTimer - Cancel a scheduled timer
static std::atomic<int> test3_counter{0};

void test3_callback(void* arg) {
    test3_counter.fetch_add(1, std::memory_order_relaxed);
    (void)arg;
}

static bool test_cancel_timer() {
    fprintf(stderr, "  Testing timer cancellation...\n");
    fflush(stderr);

    test3_counter.store(0, std::memory_order_release);

    // Schedule a timer with 200ms delay
    struct bthread_timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 200000000;  // 200ms

    bthread_timer_t timer_id = bthread_timer_add(test3_callback, nullptr, &delay);
    if (timer_id < 0) {
        fprintf(stderr, "    FAIL: Failed to schedule timer\n");
        return false;
    }

    // Cancel the timer immediately
    int ret = bthread_timer_cancel(timer_id);
    if (ret != 0) {
        fprintf(stderr, "    FAIL: Failed to cancel timer, ret=%d\n", ret);
        return false;
    }

    // Wait longer than the timer delay (300ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (test3_counter.load(std::memory_order_acquire) != 0) {
        fprintf(stderr, "    FAIL: Cancelled timer was executed, counter=%d\n",
                test3_counter.load(std::memory_order_relaxed));
        return false;
    }

    fprintf(stderr, "    PASS: Timer cancelled successfully\n");
    return true;
}

// Test 4: MixedCancelAndExecute - Mix of cancelled and executed timers
static std::atomic<int> test4_counter{0};

void test4_callback(void* arg) {
    test4_counter.fetch_add(1, std::memory_order_relaxed);
    (void)arg;
}

static bool test_mixed_cancel_and_execute() {
    fprintf(stderr, "  Testing mixed cancel and execute...\n");
    fflush(stderr);

    test4_counter.store(0, std::memory_order_release);

    struct bthread_timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 100000000;  // 100ms

    // Schedule 5 timers, cancel 3 of them
    std::vector<bthread_timer_t> timer_ids;
    for (int i = 0; i < 5; ++i) {
        bthread_timer_t timer_id = bthread_timer_add(test4_callback, nullptr, &delay);
        if (timer_id < 0) {
            fprintf(stderr, "    FAIL: Failed to schedule timer %d\n", i);
            return false;
        }
        timer_ids.push_back(timer_id);
    }

    // Cancel timers 0, 2, 4
    bthread_timer_cancel(timer_ids[0]);
    bthread_timer_cancel(timer_ids[2]);
    bthread_timer_cancel(timer_ids[4]);

    // Wait for timers to execute (200ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    int actual = test4_counter.load(std::memory_order_acquire);
    if (actual != 2) {
        fprintf(stderr, "    FAIL: Expected 2 executions, got %d\n", actual);
        return false;
    }

    fprintf(stderr, "    PASS: Correct number of timers executed (2)\n");
    return true;
}

int main() {
    // Disable buffering
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "Testing Timer Shard functionality...\n");
    fflush(stderr);

    bool all_passed = true;

    // Test 1: ScheduleAndExecute
    if (!test_schedule_and_execute()) {
        all_passed = false;
    }

    // Test 2: MultipleTimers
    if (!test_multiple_timers()) {
        all_passed = false;
    }

    // Test 3: CancelTimer
    if (!test_cancel_timer()) {
        all_passed = false;
    }

    // Test 4: MixedCancelAndExecute
    if (!test_mixed_cancel_and_execute()) {
        all_passed = false;
    }

    if (all_passed) {
        fprintf(stderr, "\nAll Timer Shard tests passed!\n");
        fflush(stderr);
        bthread_shutdown();
        return 0;
    } else {
        fprintf(stderr, "\nSome tests FAILED!\n");
        fflush(stderr);
        bthread_shutdown();
        return 1;
    }
}