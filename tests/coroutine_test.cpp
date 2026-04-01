// tests/coroutine_test.cpp
#include "coro/result.h"
#include "coro/frame_pool.h"
#include "coro/meta.h"
#include "coro/coroutine.h"
#include "coro/scheduler.h"
#include "bthread/sync/mutex.hpp"
#include "bthread/sync/cond.hpp"
#include "coro/cancel.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include <atomic>

void test_error_basic() {
    coro::Error err(1, "test error");
    assert(err.code() == 1);
    assert(err.message() == "test error");
    std::cout << "test_error_basic PASSED\n";
}

void test_result_value() {
    coro::Result<int> r(42);
    assert(r.is_ok());
    assert(!r.is_err());
    assert(r.value() == 42);
    std::cout << "test_result_value PASSED\n";
}

void test_result_error() {
    coro::Result<int> r(coro::Error(1, "failed"));
    assert(!r.is_ok());
    assert(r.is_err());
    assert(r.error().code() == 1);
    std::cout << "test_result_error PASSED\n";
}

void test_result_void() {
    coro::Result<void> ok;
    assert(ok.is_ok());
    assert(!ok.is_err());

    coro::Result<void> err(coro::Error(2, "void error"));
    assert(err.is_err());
    assert(err.error().code() == 2);
    std::cout << "test_result_void PASSED\n";
}

void test_result_value_throws_on_error() {
    coro::Result<int> r(coro::Error(1, "error"));
    try {
        r.value();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Result contains error");
    }
    std::cout << "test_result_value_throws_on_error PASSED\n";
}

void test_result_error_throws_on_success() {
    coro::Result<int> r(42);
    try {
        r.error();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Result contains value");
    }
    std::cout << "test_result_error_throws_on_success PASSED\n";
}

void test_result_void_error_throws_on_success() {
    coro::Result<void> r;
    try {
        r.error();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Result contains success");
    }
    std::cout << "test_result_void_error_throws_on_success PASSED\n";
}

void test_result_string_move_semantics() {
    // Test Result<std::string> with non-trivial type
    std::string original = "hello world";
    coro::Result<std::string> r(std::move(original));
    assert(r.is_ok());
    assert(r.value() == "hello world");

    // Test error case with string
    coro::Result<std::string> err_result(coro::Error(3, "string error"));
    assert(err_result.is_err());
    assert(err_result.error().message() == "string error");

    std::cout << "test_result_string_move_semantics PASSED\n";
}

void test_frame_pool_basic() {
    coro::FramePool pool;
    pool.Init(4 * 1024, 16);  // 4KB blocks, 16 initial

    void* block = pool.Allocate(4 * 1024);
    assert(block != nullptr);

    pool.Deallocate(block);

    // Can allocate again after deallocate
    void* block2 = pool.Allocate(4 * 1024);
    assert(block2 != nullptr);
    pool.Deallocate(block2);

    std::cout << "test_frame_pool_basic PASSED\n";
}

void test_frame_pool_expansion() {
    coro::FramePool pool;
    pool.Init(4 * 1024, 2);  // 4KB blocks, 2 initial blocks

    // Allocate more than initial count
    void* blocks[5];
    for (int i = 0; i < 5; ++i) {
        blocks[i] = pool.Allocate(4 * 1024);
        assert(blocks[i] != nullptr);
    }

    // Deallocate all
    for (int i = 0; i < 5; ++i) {
        pool.Deallocate(blocks[i]);
    }

    std::cout << "test_frame_pool_expansion PASSED\n";
}

void test_frame_pool_size_limit() {
    coro::FramePool pool;
    pool.Init(4 * 1024, 16);  // 4KB blocks

    // Request larger than block size should return nullptr
    void* block = pool.Allocate(8 * 1024);
    assert(block == nullptr);

    std::cout << "test_frame_pool_size_limit PASSED\n";
}

void test_frame_pool_uninitialized() {
    coro::FramePool pool;
    // No Init() call
    void* block = pool.Allocate(100);
    assert(block == nullptr);  // Should fail gracefully
    std::cout << "test_frame_pool_uninitialized PASSED\n";
}

void test_frame_pool_boundary() {
    coro::FramePool pool;
    pool.Init(4 * 1024, 4);

    void* block = pool.Allocate(4 * 1024);  // Exactly block_size
    assert(block != nullptr);
    pool.Deallocate(block);

    std::cout << "test_frame_pool_boundary PASSED\n";
}

void test_coroutine_queue_basic() {
    coro::CoroutineQueue queue;
    assert(queue.Empty());

    coro::CoroutineMeta meta1;
    queue.Push(&meta1);
    assert(!queue.Empty());

    coro::CoroutineMeta* popped = queue.Pop();
    assert(popped == &meta1);
    assert(queue.Empty());

    std::cout << "test_coroutine_queue_basic PASSED\n";
}

void test_coroutine_queue_multi() {
    coro::CoroutineQueue queue;

    coro::CoroutineMeta meta1, meta2, meta3;
    queue.Push(&meta1);
    queue.Push(&meta2);
    queue.Push(&meta3);

    // Pop in order
    assert(queue.Pop() == &meta1);
    assert(queue.Pop() == &meta2);
    assert(queue.Pop() == &meta3);
    assert(queue.Empty());

    std::cout << "test_coroutine_queue_multi PASSED\n";
}

void test_coroutine_meta_state() {
    coro::CoroutineMeta meta;
    assert(meta.state.load() == bthread::TaskState::READY);
    assert(meta.owner_worker == nullptr);
    assert(meta.cancel_requested.load() == false);
    assert(meta.waiting_sync == nullptr);
    assert(meta.next.load() == nullptr);
    assert(meta.slot_index == 0);
    assert(meta.generation == 0);

    // Test state transitions (atomic operations)
    meta.state.store(bthread::TaskState::RUNNING);
    assert(meta.state.load() == bthread::TaskState::RUNNING);

    meta.state.store(bthread::TaskState::SUSPENDED);
    assert(meta.state.load() == bthread::TaskState::SUSPENDED);

    meta.state.store(bthread::TaskState::FINISHED);
    assert(meta.state.load() == bthread::TaskState::FINISHED);

    std::cout << "test_coroutine_meta_state PASSED\n";
}

void test_coroutine_queue_multithreaded() {
    // Multi-producer single-consumer test
    coro::CoroutineQueue queue;
    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 100;

    std::vector<coro::CoroutineMeta> metas(NUM_PRODUCERS * ITEMS_PER_PRODUCER);

    // Producers push items from multiple threads
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &metas, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int idx = p * ITEMS_PER_PRODUCER + i;
                queue.Push(&metas[idx]);
            }
        });
    }

    // Wait for all producers
    for (auto& t : producers) {
        t.join();
    }

    // Consumer pops all items (single consumer thread)
    int count = 0;
    while (!queue.Empty()) {
        coro::CoroutineMeta* meta = queue.Pop();
        assert(meta != nullptr);
        ++count;
    }

    assert(count == NUM_PRODUCERS * ITEMS_PER_PRODUCER);
    std::cout << "test_coroutine_queue_multithreaded PASSED\n";
}

// === Task<T> tests ===

coro::Task<int> simple_coro() {
    co_return 42;
}

void test_task_basic() {
    auto task = simple_coro();
    // Directly resume (no scheduler yet)
    task.handle().resume();
    int result = task.get();
    assert(result == 42);
    std::cout << "test_task_basic PASSED\n";
}

coro::Task<void> void_coro() {
    co_return;
}

void test_task_void() {
    auto task = void_coro();
    task.handle().resume();
    task.get();  // Just verify it completes without exception
    std::cout << "test_task_void PASSED\n";
}

coro::Task<int> exception_coro() {
    throw std::runtime_error("test exception");
    co_return 0;
}

void test_task_exception() {
    auto task = exception_coro();
    task.handle().resume();
    try {
        task.get();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "test exception");
    }
    std::cout << "test_task_exception PASSED\n";
}

coro::Task<std::string> string_coro() {
    co_return "hello world";
}

void test_task_string() {
    auto task = string_coro();
    task.handle().resume();
    std::string result = task.get();
    assert(result == "hello world");
    std::cout << "test_task_string PASSED\n";
}

// === Move semantics tests ===

coro::Task<int> make_int_task() {
    co_return 123;
}

coro::Task<void> make_void_task() {
    co_return;
}

void test_task_move_constructor() {
    auto task1 = make_int_task();
    auto handle1 = task1.handle();

    // Move construct
    auto task2 = std::move(task1);

    // Original task should have null handle
    assert(!task1.handle());
    // New task should have the original handle
    assert(task2.handle() == handle1);

    // Can resume and get result from moved-to task
    task2.handle().resume();
    assert(task2.get() == 123);

    std::cout << "test_task_move_constructor PASSED\n";
}

void test_task_move_assignment() {
    auto task1 = make_int_task();
    auto handle1 = task1.handle();

    auto task2 = make_int_task();
    task2.handle().resume();

    // Move assign - task2's old handle should be destroyed
    task2 = std::move(task1);

    // Original task should have null handle
    assert(!task1.handle());
    // New task should have the original handle
    assert(task2.handle() == handle1);

    // Can resume and get result
    task2.handle().resume();
    assert(task2.get() == 123);

    std::cout << "test_task_move_assignment PASSED\n";
}

void test_task_self_assignment() {
    auto task = make_int_task();
    auto handle = task.handle();

    // Self-assignment should be a no-op
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    task = std::move(task);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    // Handle should still be valid
    assert(task.handle() == handle);

    // Can still use the task
    task.handle().resume();
    assert(task.get() == 123);

    std::cout << "test_task_self_assignment PASSED\n";
}

void test_task_void_move_constructor() {
    auto task1 = make_void_task();
    auto handle1 = task1.handle();

    // Move construct
    auto task2 = std::move(task1);

    // Original task should have null handle
    assert(!task1.handle());
    // New task should have the original handle
    assert(task2.handle() == handle1);

    // Can resume and get result from moved-to task
    task2.handle().resume();
    task2.get();  // Should not throw

    std::cout << "test_task_void_move_constructor PASSED\n";
}

void test_task_void_move_assignment() {
    auto task1 = make_void_task();
    auto handle1 = task1.handle();

    auto task2 = make_void_task();
    task2.handle().resume();

    // Move assign - task2's old handle should be destroyed
    task2 = std::move(task1);

    // Original task should have null handle
    assert(!task1.handle());
    // New task should have the original handle
    assert(task2.handle() == handle1);

    // Can resume and get result
    task2.handle().resume();
    task2.get();  // Should not throw

    std::cout << "test_task_void_move_assignment PASSED\n";
}

void test_task_void_self_assignment() {
    auto task = make_void_task();
    auto handle = task.handle();

    // Self-assignment should be a no-op
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
    task = std::move(task);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

    // Handle should still be valid
    assert(task.handle() == handle);

    // Can still use the task
    task.handle().resume();
    task.get();  // Should not throw

    std::cout << "test_task_void_self_assignment PASSED\n";
}

// === Error path tests ===

void test_task_get_on_incomplete_throws() {
    auto task = make_int_task();
    // Don't resume - task is incomplete

    try {
        task.get();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Task not completed");
    }

    std::cout << "test_task_get_on_incomplete_throws PASSED\n";
}

void test_task_get_on_moved_from_throws() {
    auto task1 = make_int_task();
    auto task2 = std::move(task1);

    // task1 is now moved-from
    try {
        task1.get();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Task has no handle");
    }

    // Clean up task2
    task2.handle().resume();
    task2.get();

    std::cout << "test_task_get_on_moved_from_throws PASSED\n";
}

void test_task_void_get_on_incomplete_throws() {
    auto task = make_void_task();
    // Don't resume - task is incomplete

    try {
        task.get();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Task not completed");
    }

    std::cout << "test_task_void_get_on_incomplete_throws PASSED\n";
}

void test_task_void_get_on_moved_from_throws() {
    auto task1 = make_void_task();
    auto task2 = std::move(task1);

    // task1 is now moved-from
    try {
        task1.get();
        assert(false && "Expected exception not thrown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Task has no handle");
    }

    // Clean up task2
    task2.handle().resume();
    task2.get();

    std::cout << "test_task_void_get_on_moved_from_throws PASSED\n";
}

// === Scheduler tests ===

coro::Task<int> spawn_test_coro() {
    co_return 100;
}

void test_scheduler_spawn_and_wait() {
    auto task = coro::co_spawn(spawn_test_coro());

    // Wait for completion (polling)
    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 100);
    std::cout << "test_scheduler_spawn_and_wait PASSED\n";
}

coro::Task<int> multi_spawn_coro(int value) {
    co_return value * 2;
}

void test_scheduler_multi_spawn() {
    auto t1 = coro::co_spawn(multi_spawn_coro(10));
    auto t2 = coro::co_spawn(multi_spawn_coro(20));
    auto t3 = coro::co_spawn(multi_spawn_coro(30));

    // Wait for all to complete
    while (!t1.is_done() || !t2.is_done() || !t3.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(t1.get() == 20);
    assert(t2.get() == 40);
    assert(t3.get() == 60);
    std::cout << "test_scheduler_multi_spawn PASSED\n";
}

coro::Task<void> void_spawn_coro(int& counter) {
    counter++;
    co_return;
}

void test_scheduler_void_task() {
    int counter = 0;
    auto task = coro::co_spawn(void_spawn_coro(counter));

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    task.get();  // Should complete without exception
    assert(counter == 1);
    std::cout << "test_scheduler_void_task PASSED\n";
}

// === Concurrent spawn test ===

coro::Task<int> concurrent_spawn_coro(int id, int delay_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    co_return id * 2;
}

void test_scheduler_concurrent_spawns() {
    constexpr int NUM_THREADS = 4;
    constexpr int TASKS_PER_THREAD = 10;
    std::vector<std::thread> threads;
    std::vector<std::vector<coro::Task<int>>> all_tasks(NUM_THREADS);

    // Spawn tasks from multiple threads concurrently
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&all_tasks, t]() {
            for (int i = 0; i < TASKS_PER_THREAD; ++i) {
                all_tasks[t].push_back(coro::co_spawn(concurrent_spawn_coro(t * 100 + i, 5)));
            }
        });
    }

    // Wait for all spawn threads to complete
    for (auto& t : threads) {
        t.join();
    }

    // Wait for all tasks to complete and verify results
    int total_completed = 0;
    for (int t = 0; t < NUM_THREADS; ++t) {
        for (int i = 0; i < TASKS_PER_THREAD; ++i) {
            while (!all_tasks[t][i].is_done()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int expected = (t * 100 + i) * 2;
            assert(all_tasks[t][i].get() == expected);
            ++total_completed;
        }
    }

    assert(total_completed == NUM_THREADS * TASKS_PER_THREAD);
    std::cout << "test_scheduler_concurrent_spawns PASSED\n";
}

// === Exception handling test ===

coro::Task<int> throwing_coro(int value) {
    if (value < 0) {
        throw std::runtime_error("negative value not allowed");
    }
    co_return value * 2;
}

void test_scheduler_exception_in_coroutine() {
    // Test that a throwing coroutine doesn't crash the scheduler
    auto throwing_task = coro::co_spawn(throwing_coro(-1));
    auto normal_task = coro::co_spawn(throwing_coro(10));

    // Wait for both tasks to complete (the throwing one should be handled)
    int max_wait = 100;  // max 1 second
    while ((!throwing_task.is_done() || !normal_task.is_done()) && max_wait > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        --max_wait;
    }

    // The normal task should complete successfully
    assert(normal_task.is_done());
    assert(normal_task.get() == 20);

    // The throwing task should also be marked as done (even though it threw)
    // Note: The exception is caught by the worker thread and logged,
    // and the task is cleaned up. The result may be undefined or throw.
    assert(throwing_task.is_done());

    std::cout << "test_scheduler_exception_in_coroutine PASSED\n";
}

// === CoMutex tests ===

coro::Task<int> mutex_test_coro(bthread::Mutex& m, int& counter) {
    co_await m.lock_async();
    counter++;
    m.unlock();
    co_return counter;
}

void test_comutex_basic() {
    bthread::Mutex mutex;
    int counter = 0;

    auto t1 = coro::co_spawn(mutex_test_coro(mutex, counter));
    auto t2 = coro::co_spawn(mutex_test_coro(mutex, counter));

    while (!t1.is_done() || !t2.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(counter == 2);
    std::cout << "test_comutex_basic PASSED\n";
}

void test_comutex_try_lock() {
    bthread::Mutex mutex;

    // Should succeed initially
    assert(mutex.try_lock());

    // Should fail now (locked)
    assert(!mutex.try_lock());

    // Unlock and try again
    mutex.unlock();
    assert(mutex.try_lock());
    mutex.unlock();

    std::cout << "test_comutex_try_lock PASSED\n";
}

coro::Task<void> mutex_contention_coro(bthread::Mutex& m, std::atomic<int>& counter, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        co_await m.lock_async();
        counter.fetch_add(1);
        m.unlock();
    }
}

void test_comutex_contention() {
    bthread::Mutex mutex;
    std::atomic<int> counter{0};
    constexpr int ITERATIONS = 50;
    constexpr int NUM_COROS = 4;

    std::vector<coro::Task<void>> tasks;
    for (int i = 0; i < NUM_COROS; ++i) {
        tasks.push_back(coro::co_spawn(mutex_contention_coro(mutex, counter, ITERATIONS)));
    }

    // Wait for all tasks to complete
    for (auto& t : tasks) {
        while (!t.is_done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        t.get();  // Complete without exception
    }

    // Total increments should be NUM_COROS * ITERATIONS
    assert(counter.load() == NUM_COROS * ITERATIONS);
    std::cout << "test_comutex_contention PASSED\n";
}

coro::Task<int> mutex_nested_lock_coro(bthread::Mutex& m1, bthread::Mutex& m2, int& value) {
    co_await m1.lock_async();
    co_await m2.lock_async();
    value += 100;
    m2.unlock();
    m1.unlock();
    co_return value;
}

void test_comutex_nested_locks() {
    bthread::Mutex mutex1;
    bthread::Mutex mutex2;
    int value = 0;

    auto task = coro::co_spawn(mutex_nested_lock_coro(mutex1, mutex2, value));

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 100);
    assert(value == 100);
    std::cout << "test_comutex_nested_locks PASSED\n";
}

coro::Task<int> mutex_return_value_coro(bthread::Mutex& m, int input) {
    co_await m.lock_async();
    int result = input * 2;
    m.unlock();
    co_return result;
}

void test_comutex_with_value() {
    bthread::Mutex mutex;

    auto t1 = coro::co_spawn(mutex_return_value_coro(mutex, 10));
    auto t2 = coro::co_spawn(mutex_return_value_coro(mutex, 20));

    while (!t1.is_done() || !t2.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(t1.get() == 20);
    assert(t2.get() == 40);
    std::cout << "test_comutex_with_value PASSED\n";
}

// === CoCond tests ===

coro::Task<void> producer(bthread::Mutex& m, bthread::CondVar& c, int& data) {
    co_await m.lock_async();
    data = 42;
    c.notify_one();
    m.unlock();
}

coro::Task<void> consumer(bthread::Mutex& m, bthread::CondVar& c, int& data) {
    co_await m.lock_async();
    while (data == 0) {
        co_await c.wait_async(m);
    }
    assert(data == 42);
    m.unlock();
}

void test_cocond_basic() {
    bthread::Mutex mutex;
    bthread::CondVar cond;
    int data = 0;

    auto c = coro::co_spawn(consumer(mutex, cond, data));
    auto p = coro::co_spawn(producer(mutex, cond, data));

    while (!c.is_done() || !p.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    c.get();  // Complete without exception
    p.get();  // Complete without exception
    assert(data == 42);
    std::cout << "test_cocond_basic PASSED\n";
}

coro::Task<void> broadcaster(bthread::Mutex& m, bthread::CondVar& c, std::atomic<int>& ready_count) {
    co_await m.lock_async();
    // Wait until all consumers are ready
    while (ready_count.load() < 3) {
        co_await c.wait_async(m);
    }
    // Broadcast to wake all waiting consumers
    c.notify_all();
    m.unlock();
}

coro::Task<void> consumer_for_broadcast(bthread::Mutex& m, bthread::CondVar& c, std::atomic<int>& ready_count, int& data) {
    co_await m.lock_async();
    ready_count.fetch_add(1);
    c.notify_one();  // Signal that we're ready
    co_await c.wait_async(m);  // Wait for broadcast
    data++;  // Increment after broadcast wakes us
    m.unlock();
}

void test_cocond_broadcast() {
    bthread::Mutex mutex;
    bthread::CondVar cond;
    std::atomic<int> ready_count{0};
    int data = 0;

    auto b = coro::co_spawn(broadcaster(mutex, cond, ready_count));
    auto c1 = coro::co_spawn(consumer_for_broadcast(mutex, cond, ready_count, data));
    auto c2 = coro::co_spawn(consumer_for_broadcast(mutex, cond, ready_count, data));
    auto c3 = coro::co_spawn(consumer_for_broadcast(mutex, cond, ready_count, data));

    while (!b.is_done() || !c1.is_done() || !c2.is_done() || !c3.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    b.get();
    c1.get();
    c2.get();
    c3.get();
    assert(data == 3);  // All consumers should have incremented
    std::cout << "test_cocond_broadcast PASSED\n";
}

coro::Task<void> multi_producer(bthread::Mutex& m, bthread::CondVar& c, int& data, int id) {
    co_await m.lock_async();
    data += id;
    c.notify_one();  // Signal after each production
    m.unlock();
}

coro::Task<void> multi_consumer(bthread::Mutex& m, bthread::CondVar& c, int& data, int expected_sum) {
    co_await m.lock_async();
    while (data < expected_sum) {
        co_await c.wait_async(m);
    }
    m.unlock();
}

void test_cocond_multiple_signal() {
    bthread::Mutex mutex;
    bthread::CondVar cond;
    int data = 0;
    int expected_sum = 10 + 20 + 30;  // Sum of producer IDs

    auto consumer_task = coro::co_spawn(multi_consumer(mutex, cond, data, expected_sum));
    auto p1 = coro::co_spawn(multi_producer(mutex, cond, data, 10));
    auto p2 = coro::co_spawn(multi_producer(mutex, cond, data, 20));
    auto p3 = coro::co_spawn(multi_producer(mutex, cond, data, 30));

    while (!consumer_task.is_done() || !p1.is_done() || !p2.is_done() || !p3.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    consumer_task.get();
    p1.get();
    p2.get();
    p3.get();
    assert(data == expected_sum);
    std::cout << "test_cocond_multiple_signal PASSED\n";
}

coro::Task<void> signal_no_waiter_coro(bthread::CondVar& c) {
    c.notify_one();  // Signal when no one is waiting - should be safe
    co_return;
}

void test_cocond_signal_no_waiter() {
    bthread::CondVar cond;

    auto task = coro::co_spawn(signal_no_waiter_coro(cond));

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    task.get();  // Should complete without exception
    std::cout << "test_cocond_signal_no_waiter PASSED\n";
}

coro::Task<void> broadcast_no_waiter_coro(bthread::CondVar& c) {
    c.notify_all();  // Broadcast when no one is waiting - should be safe
    co_return;
}

void test_cocond_broadcast_no_waiter() {
    bthread::CondVar cond;

    auto task = coro::co_spawn(broadcast_no_waiter_coro(cond));

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    task.get();  // Should complete without exception
    std::cout << "test_cocond_broadcast_no_waiter PASSED\n";
}

// Test for concurrent signal() from multiple threads
coro::Task<void> concurrent_signal_producer(bthread::Mutex& m, bthread::CondVar& c, std::atomic<int>& counter) {
    co_await m.lock_async();
    counter.fetch_add(1);
    c.notify_one();
    m.unlock();
}

void test_cocond_concurrent_signal() {
    // Test that concurrent signal() calls from multiple threads are safe
    // and don't cause data races or crashes.
    // This is a stress test rather than a functional test - we just verify
    // that the implementation handles concurrent calls correctly.
    bthread::Mutex mutex;
    bthread::CondVar cond;
    std::atomic<int> counter{0};
    constexpr int NUM_PRODUCERS = 8;

    // Spawn all producers from different OS threads concurrently
    std::vector<std::thread> producer_threads;
    std::atomic<int> ready_count{0};
    std::atomic<bool> go{false};

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        producer_threads.emplace_back([&mutex, &cond, &counter, &ready_count, &go]() {
            // Synchronize all threads to start at the same time
            ready_count.fetch_add(1);
            while (!go.load()) {
                std::this_thread::yield();
            }
            // Spawn producer coroutine
            auto task = coro::co_spawn(concurrent_signal_producer(mutex, cond, counter));
            // Wait for completion
            while (!task.is_done()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            task.get();
        });
    }

    // Wait for all threads to be ready
    while (ready_count.load() < NUM_PRODUCERS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Signal all threads to start simultaneously
    go.store(true);

    // Wait for all producer threads to complete
    for (auto& t : producer_threads) {
        t.join();
    }

    // Verify all producers completed successfully
    assert(counter.load() == NUM_PRODUCERS);
    std::cout << "test_cocond_concurrent_signal PASSED\n";
}

// === Cancellation tests ===
// NOTE: CancellationToken stores a raw pointer to CancelSource's internal state.
// IMPORTANT: The token MUST NOT outlive the CancelSource that created it.
// If the CancelSource is destroyed while a token is still in use, accessing
// the token causes undefined behavior (dangling pointer dereference).
// In these tests, CancelSource always outlives the token and coroutine.

void test_cancel_source_basic() {
    coro::CancelSource source;

    // Initially not cancelled
    assert(!source.is_cancelled());

    // Get token
    coro::CancellationToken token = source.token();
    assert(!token.is_cancelled());

    // Request cancellation
    source.cancel();
    assert(source.is_cancelled());
    assert(token.is_cancelled());

    // Reset
    source.reset();
    assert(!source.is_cancelled());
    assert(!token.is_cancelled());

    std::cout << "test_cancel_source_basic PASSED\n";
}

void test_cancel_token_multiple() {
    coro::CancelSource source;
    coro::CancellationToken token1 = source.token();
    coro::CancellationToken token2 = source.token();

    // Both tokens share the same state
    assert(!token1.is_cancelled());
    assert(!token2.is_cancelled());

    source.cancel();
    assert(token1.is_cancelled());
    assert(token2.is_cancelled());

    std::cout << "test_cancel_token_multiple PASSED\n";
}

coro::Task<int> cancelable_coro(coro::CancellationToken token) {
    int count = 0;
    while (count < 10000) {  // Large iteration count to ensure it doesn't complete quickly
        bool cancelled = co_await token.check_cancel();
        if (cancelled) {
            co_return -1;  // Cancelled
        }
        count++;
        co_await coro::yield();
    }
    co_return count;
}

void test_cancellation_in_coroutine() {
    coro::CancelSource source;
    auto task = coro::co_spawn(cancelable_coro(source.token()));

    // Cancel almost immediately
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    source.cancel();

    // Wait for completion
    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Should be cancelled (return -1) - yield() ensures the coroutine doesn't
    // complete 1000 iterations in 10ms
    int result = task.get();
    assert(result == -1);  // Was cancelled

    std::cout << "test_cancellation_in_coroutine PASSED\n";
}

coro::Task<int> non_cancelable_coro(coro::CancellationToken token) {
    int count = 0;
    while (count < 10) {
        // Check cancel but don't stop
        bool cancelled = co_await token.check_cancel();
        // Continue even if cancelled (demonstration)
        count++;
        co_await coro::yield();
    }
    co_return count;
}

void test_cancellation_ignore() {
    coro::CancelSource source;
    auto task = coro::co_spawn(non_cancelable_coro(source.token()));

    // Cancel immediately
    source.cancel();

    // Wait for completion - coroutine ignores cancellation
    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 10);  // Completed fully

    std::cout << "test_cancellation_ignore PASSED\n";
}

// === Yield tests ===

coro::Task<int> yield_test_coro(int iterations) {
    int count = 0;
    for (int i = 0; i < iterations; ++i) {
        count++;
        co_await coro::yield();
    }
    co_return count;
}

void test_yield_basic() {
    auto task = coro::co_spawn(yield_test_coro(5));

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 5);

    std::cout << "test_yield_basic PASSED\n";
}

coro::Task<int> yield_with_counter_coro(std::atomic<int>& counter) {
    for (int i = 0; i < 10; ++i) {
        counter.fetch_add(1);
        co_await coro::yield();
    }
    co_return counter.load();
}

void test_yield_multiple_coroutines() {
    std::atomic<int> counter{0};

    auto t1 = coro::co_spawn(yield_with_counter_coro(counter));
    auto t2 = coro::co_spawn(yield_with_counter_coro(counter));
    auto t3 = coro::co_spawn(yield_with_counter_coro(counter));

    while (!t1.is_done() || !t2.is_done() || !t3.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // All 3 coroutines should have run 10 iterations each (total 30)
    // Note: Each task returns counter.load() at its completion time,
    // which may differ based on scheduling. But the final counter must be 30.
    assert(counter.load() == 30);

    // Verify each task completed successfully (returned some value >= 10)
    assert(t1.get() >= 10);
    assert(t2.get() >= 10);
    assert(t3.get() >= 10);

    std::cout << "test_yield_multiple_coroutines PASSED\n";
}

// === SafeTask tests ===

coro::SafeTask<int> safe_value_coro() {
    co_return 100;
}

coro::SafeTask<int> safe_error_coro() {
    co_return coro::Error(5, "test error");
}

coro::SafeTask<std::string> safe_string_coro() {
    co_return "hello safe world";
}

coro::SafeTask<int> safe_exception_coro() {
    throw std::runtime_error("safe exception test");
    co_return 0;
}

coro::SafeTask<int> safe_unknown_exception_coro() {
    throw 42;  // Non-std::exception
    co_return 0;
}

void test_safetask_basic() {
    auto task = safe_value_coro();
    task.handle().resume();
    coro::Result<int> result = task.get();
    assert(result.is_ok());
    assert(result.value() == 100);
    std::cout << "test_safetask_basic PASSED\n";
}

void test_safetask_error() {
    auto task = safe_error_coro();
    task.handle().resume();
    coro::Result<int> result = task.get();
    assert(result.is_err());
    assert(result.error().code() == 5);
    assert(result.error().message() == "test error");
    std::cout << "test_safetask_error PASSED\n";
}

void test_safetask_string() {
    auto task = safe_string_coro();
    task.handle().resume();
    coro::Result<std::string> result = task.get();
    assert(result.is_ok());
    assert(result.value() == "hello safe world");
    std::cout << "test_safetask_string PASSED\n";
}

void test_safetask_exception() {
    auto task = safe_exception_coro();
    task.handle().resume();
    coro::Result<int> result = task.get();
    assert(result.is_err());
    assert(result.error().code() == -3);  // std::exception error code
    assert(result.error().message() == "safe exception test");
    std::cout << "test_safetask_exception PASSED\n";
}

void test_safetask_unknown_exception() {
    auto task = safe_unknown_exception_coro();
    task.handle().resume();
    coro::Result<int> result = task.get();
    assert(result.is_err());
    assert(result.error().code() == -4);  // unknown exception error code
    assert(result.error().message() == "unknown exception");
    std::cout << "test_safetask_unknown_exception PASSED\n";
}

// === SafeTask<void> tests ===

coro::SafeTask<void> safe_void_coro() {
    co_return coro::VoidSuccess{};  // Success
}

coro::SafeTask<void> safe_void_error_coro() {
    co_return coro::Error(10, "void error");
}

coro::SafeTask<void> safe_void_exception_coro() {
    throw std::runtime_error("void exception");
    co_return coro::VoidSuccess{};  // Needed for compilation
}

void test_safetask_void_basic() {
    auto task = safe_void_coro();
    task.handle().resume();
    coro::Result<void> result = task.get();
    assert(result.is_ok());
    std::cout << "test_safetask_void_basic PASSED\n";
}

void test_safetask_void_error() {
    auto task = safe_void_error_coro();
    task.handle().resume();
    coro::Result<void> result = task.get();
    assert(result.is_err());
    assert(result.error().code() == 10);
    assert(result.error().message() == "void error");
    std::cout << "test_safetask_void_error PASSED\n";
}

void test_safetask_void_exception() {
    auto task = safe_void_exception_coro();
    task.handle().resume();
    coro::Result<void> result = task.get();
    assert(result.is_err());
    assert(result.error().code() == -3);
    assert(result.error().message() == "void exception");
    std::cout << "test_safetask_void_exception PASSED\n";
}

// === SafeTask edge cases ===

void test_safetask_no_handle() {
    // Create a moved-from task with no handle
    auto task1 = safe_value_coro();
    auto task2 = std::move(task1);

    // task1 now has no handle
    coro::Result<int> result = task1.get();
    assert(result.is_err());
    assert(result.error().code() == -1);
    assert(result.error().message() == "Task has no handle");

    // Clean up task2
    task2.handle().resume();
    (void)task2.get();

    std::cout << "test_safetask_no_handle PASSED\n";
}

void test_safetask_not_completed() {
    auto task = safe_value_coro();
    // Don't resume - task is incomplete

    coro::Result<int> result = task.get();
    assert(result.is_err());
    assert(result.error().code() == -2);
    assert(result.error().message() == "Task not completed");

    // Clean up by resuming
    task.handle().resume();
    (void)task.get();

    std::cout << "test_safetask_not_completed PASSED\n";
}

// === SafeTask move semantics ===

coro::SafeTask<int> make_safe_int_task() {
    co_return 123;
}

coro::SafeTask<void> make_safe_void_task() {
    co_return coro::VoidSuccess{};
}

void test_safetask_move_constructor() {
    auto task1 = make_safe_int_task();
    auto handle1 = task1.handle();

    auto task2 = std::move(task1);

    assert(!task1.handle());
    assert(task2.handle() == handle1);

    task2.handle().resume();
    assert(task2.get().is_ok());
    assert(task2.get().value() == 123);

    std::cout << "test_safetask_move_constructor PASSED\n";
}

void test_safetask_move_assignment() {
    auto task1 = make_safe_int_task();
    auto handle1 = task1.handle();

    auto task2 = make_safe_int_task();
    task2.handle().resume();

    task2 = std::move(task1);

    assert(!task1.handle());
    assert(task2.handle() == handle1);

    task2.handle().resume();
    assert(task2.get().is_ok());
    assert(task2.get().value() == 123);

    std::cout << "test_safetask_move_assignment PASSED\n";
}

void test_safetask_void_move() {
    auto task1 = make_safe_void_task();
    auto handle1 = task1.handle();

    auto task2 = std::move(task1);

    assert(!task1.handle());
    assert(task2.handle() == handle1);

    task2.handle().resume();
    assert(task2.get().is_ok());

    std::cout << "test_safetask_void_move PASSED\n";
}

// === SafeTask co_await test ===

coro::Task<int> await_safe_task_coro() {
    // Create SafeTask and manually resume it before co_await
    auto safe_task = safe_value_coro();
    safe_task.handle().resume();

    // co_await on completed SafeTask returns Result<int>
    coro::Result<int> result = co_await safe_task;
    if (result.is_ok()) {
        co_return result.value() + 1;  // 100 + 1 = 101
    }
    co_return -1;
}

void test_safetask_co_await() {
    auto outer_task = await_safe_task_coro();
    outer_task.handle().resume();

    assert(outer_task.is_done());
    assert(outer_task.get() == 101);

    std::cout << "test_safetask_co_await PASSED\n";
}

coro::Task<int> await_safe_error_coro() {
    auto safe_task = safe_error_coro();
    safe_task.handle().resume();

    coro::Result<int> result = co_await safe_task;
    if (result.is_err()) {
        co_return result.error().code();
    }
    co_return 0;
}

void test_safetask_co_await_error() {
    auto outer_task = await_safe_error_coro();
    outer_task.handle().resume();

    assert(outer_task.is_done());
    assert(outer_task.get() == 5);

    std::cout << "test_safetask_co_await_error PASSED\n";
}

// === SafeTask with scheduler test ===

coro::SafeTask<int> safe_scheduler_coro(int value) {
    co_return value * 3;
}

void test_safetask_scheduler_spawn() {
    auto task = coro::co_spawn(safe_scheduler_coro(15));

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    coro::Result<int> result = task.get();
    assert(result.is_ok());
    assert(result.value() == 45);

    std::cout << "test_safetask_scheduler_spawn PASSED\n";
}

coro::SafeTask<void> safe_scheduler_void_coro(std::atomic<int>& counter) {
    counter.fetch_add(1);
    co_return coro::VoidSuccess{};
}

void test_safetask_scheduler_void_spawn() {
    std::atomic<int> counter{0};
    auto task = coro::co_spawn(safe_scheduler_void_coro(counter));

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    coro::Result<void> result = task.get();
    assert(result.is_ok());
    assert(counter.load() == 1);

    std::cout << "test_safetask_scheduler_void_spawn PASSED\n";
}

// === Sleep tests ===

coro::Task<int> sleep_coro() {
    auto start = std::chrono::steady_clock::now();
    co_await coro::sleep(std::chrono::milliseconds(100));
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    co_return static_cast<int>(elapsed);
}

void test_sleep() {
    auto task = coro::co_spawn(sleep_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int elapsed = task.get();
    assert(elapsed >= 100);  // At least 100ms passed
    assert(elapsed < 200);   // But not too long (allow some tolerance)

    std::cout << "test_sleep PASSED\n";
}

coro::Task<int> multiple_sleep_coro() {
    int total = 0;
    for (int i = 0; i < 3; ++i) {
        auto start = std::chrono::steady_clock::now();
        co_await coro::sleep(std::chrono::milliseconds(50));
        auto end = std::chrono::steady_clock::now();
        total += static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    }
    co_return total;
}

void test_sleep_multiple() {
    auto task = coro::co_spawn(multiple_sleep_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int total = task.get();
    assert(total >= 150);  // 3 * 50ms = 150ms minimum
    assert(total < 250);   // Allow some tolerance

    std::cout << "test_sleep_multiple PASSED\n";
}

coro::Task<int> sleep_with_yield_coro() {
    auto start = std::chrono::steady_clock::now();
    co_await coro::sleep(std::chrono::milliseconds(30));
    co_await coro::yield();  // Yield after sleep
    co_await coro::sleep(std::chrono::milliseconds(30));
    auto end = std::chrono::steady_clock::now();
    co_return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

void test_sleep_with_yield() {
    auto task = coro::co_spawn(sleep_with_yield_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int elapsed = task.get();
    assert(elapsed >= 60);  // At least 60ms (30 + 30)
    assert(elapsed < 150);  // Allow some tolerance for yield

    std::cout << "test_sleep_with_yield PASSED\n";
}

// === Auto-initialization test ===
// This test must run FIRST before any other scheduler tests

void test_scheduler_auto_init() {
    // Spawn without explicit Init() - should auto-initialize
    auto task = coro::co_spawn(simple_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 42);
    std::cout << "test_scheduler_auto_init PASSED\n";
}

// === Integration tests (Task 11) ===

// Stress test - many coroutines contending for mutex
coro::Task<void> stress_worker(bthread::Mutex& m, std::atomic<int>& counter, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        co_await m.lock_async();
        counter++;
        m.unlock();
        co_await coro::yield();
    }
}

void test_stress_mutex_contention() {
    bthread::Mutex mutex;
    std::atomic<int> counter{0};
    constexpr int ITERATIONS = 50;  // Reduced for faster testing
    constexpr int WORKERS = 5;

    std::vector<coro::Task<void>> tasks;
    for (int i = 0; i < WORKERS; ++i) {
        tasks.push_back(coro::co_spawn(stress_worker(mutex, counter, ITERATIONS)));
    }

    // Wait for all tasks to complete
    for (auto& t : tasks) {
        while (!t.is_done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        t.get();  // Complete without exception
    }

    // Verify all increments completed correctly
    assert(counter.load() == WORKERS * ITERATIONS);
    std::cout << "test_stress_mutex_contention PASSED\n";
}

// Nested coroutines - co_await co_spawn(inner_coro())
coro::Task<int> nested_inner_coro() {
    co_await coro::yield();
    co_return 10;
}

coro::Task<int> nested_outer_coro() {
    // Spawn inner coroutine and wait for its result
    int inner_result = co_await coro::co_spawn(nested_inner_coro());
    co_return inner_result + 5;  // Should return 15
}

void test_nested_coroutines() {
    auto task = coro::co_spawn(nested_outer_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 15);  // inner (10) + outer adds 5
    std::cout << "test_nested_coroutines PASSED\n";
}

// Deeply nested coroutines - multiple levels
coro::Task<int> deep_level3_coro() {
    co_return 1;
}

coro::Task<int> deep_level2_coro() {
    int r = co_await coro::co_spawn(deep_level3_coro());
    co_return r + 2;
}

coro::Task<int> deep_level1_coro() {
    int r = co_await coro::co_spawn(deep_level2_coro());
    co_return r + 3;
}

void test_deeply_nested_coroutines() {
    auto task = coro::co_spawn(deep_level1_coro());

    while (!task.is_done()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(task.get() == 6);  // 1 + 2 + 3
    std::cout << "test_deeply_nested_coroutines PASSED\n";
}

// Detached coroutines - fire-and-forget
std::atomic<int> detached_counter{0};

coro::Task<void> detached_worker_coro(int id) {
    co_await coro::sleep(std::chrono::milliseconds(30));
    detached_counter.fetch_add(1);
    co_return;
}

void test_detached_coroutines() {
    detached_counter.store(0);

    // Spawn detached coroutines (fire-and-forget)
    constexpr int NUM_DETACHED = 3;
    for (int i = 0; i < NUM_DETACHED; ++i) {
        coro::co_spawn_detached(detached_worker_coro(i));
    }

    // Wait for them to complete by polling
    int max_wait = 100;  // max 1 second
    while (detached_counter.load() < NUM_DETACHED && max_wait > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        --max_wait;
    }

    assert(detached_counter.load() == NUM_DETACHED);
    std::cout << "test_detached_coroutines PASSED\n";
}

// Detached coroutines with mutex - verify proper synchronization
std::atomic<int> detached_mutex_counter{0};

coro::Task<void> detached_mutex_coro(bthread::Mutex& m, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        co_await m.lock_async();
        detached_mutex_counter.fetch_add(1);
        m.unlock();
        co_await coro::yield();
    }
}

void test_detached_coroutines_with_mutex() {
    detached_mutex_counter.store(0);
    bthread::Mutex mutex;
    constexpr int NUM_DETACHED = 3;
    constexpr int ITERATIONS = 15;

    for (int i = 0; i < NUM_DETACHED; ++i) {
        coro::co_spawn_detached(detached_mutex_coro(mutex, ITERATIONS));
    }

    // Wait for completion
    int max_wait = 200;  // max 2 seconds
    while (detached_mutex_counter.load() < NUM_DETACHED * ITERATIONS && max_wait > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        --max_wait;
    }

    assert(detached_mutex_counter.load() == NUM_DETACHED * ITERATIONS);
    std::cout << "test_detached_coroutines_with_mutex PASSED\n";
}

// Stress test with mixed operations
coro::Task<void> mixed_stress_worker(bthread::Mutex& m, bthread::CondVar& c,
                                      std::atomic<int>& counter, int id) {
    co_await m.lock_async();
    counter.fetch_add(1);
    c.notify_one();  // Signal after first increment
    m.unlock();

    // Do some sleep/yield operations
    co_await coro::sleep(std::chrono::milliseconds(10));
    co_await coro::yield();

    // Another mutex lock and increment
    co_await m.lock_async();
    counter.fetch_add(1);
    c.notify_one();  // Signal after second increment too!
    m.unlock();
}

coro::Task<void> mixed_stress_waiter(bthread::Mutex& m, bthread::CondVar& c,
                                      std::atomic<int>& counter, int target) {
    co_await m.lock_async();
    while (counter.load() < target) {
        co_await c.wait_async(m);
    }
    m.unlock();
}

void test_stress_mixed_operations() {
    // Simplified stress test - just verify concurrent mutex operations work
    bthread::Mutex mutex;
    bthread::CondVar cond;
    std::atomic<int> counter{0};
    constexpr int NUM_WORKERS = 3;
    constexpr int TARGET = NUM_WORKERS * 2;  // Each worker increments twice

    // Workers that increment twice with mutex and signal
    auto worker = [&mutex, &cond, &counter]() -> coro::Task<void> {
        co_await mutex.lock_async();
        counter.fetch_add(1);
        cond.notify_one();
        mutex.unlock();

        co_await coro::yield();

        co_await mutex.lock_async();
        counter.fetch_add(1);
        cond.notify_one();
        mutex.unlock();
    };

    // Start workers
    std::vector<coro::Task<void>> workers;
    for (int i = 0; i < NUM_WORKERS; ++i) {
        workers.push_back(coro::co_spawn(worker()));
    }

    // Wait for all workers to complete
    for (auto& t : workers) {
        while (!t.is_done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        t.get();
    }

    assert(counter.load() == TARGET);
    std::cout << "test_stress_mixed_operations PASSED\n";
}

int main() {
    // Disable buffering for immediate output
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    test_error_basic();
    test_result_value();
    test_result_error();
    test_result_void();
    test_result_value_throws_on_error();
    test_result_error_throws_on_success();
    test_result_void_error_throws_on_success();
    test_result_string_move_semantics();
    test_frame_pool_basic();
    test_frame_pool_expansion();
    test_frame_pool_size_limit();
    test_frame_pool_uninitialized();
    test_frame_pool_boundary();
    test_coroutine_queue_basic();
    test_coroutine_queue_multi();
    test_coroutine_meta_state();
    test_coroutine_queue_multithreaded();
    test_task_basic();
    test_task_void();
    test_task_exception();
    test_task_string();
    test_task_move_constructor();
    test_task_move_assignment();
    test_task_self_assignment();
    test_task_void_move_constructor();
    test_task_void_move_assignment();
    test_task_void_self_assignment();
    test_task_get_on_incomplete_throws();
    test_task_get_on_moved_from_throws();
    test_task_void_get_on_incomplete_throws();
    test_task_void_get_on_moved_from_throws();

    // Auto-init test must run FIRST before explicit Init()
    // Note: Init() is called automatically by first Spawn()
    test_scheduler_auto_init();

    // Now the scheduler is initialized, run remaining scheduler tests
    test_scheduler_spawn_and_wait();
    test_scheduler_multi_spawn();
    test_scheduler_void_task();
    test_scheduler_concurrent_spawns();
    test_scheduler_exception_in_coroutine();

    // Run sleep tests (scheduler is still running) - before CoMutex tests
    test_sleep();
    test_sleep_multiple();
    test_sleep_with_yield();

    // Run CoMutex tests (scheduler is still running)
    test_comutex_basic();
    test_comutex_try_lock();
    test_comutex_contention();
    test_comutex_nested_locks();
    test_comutex_with_value();

    // Run CoCond tests (scheduler is still running)
    test_cocond_basic();
    test_cocond_broadcast();
    test_cocond_multiple_signal();
    test_cocond_signal_no_waiter();
    test_cocond_broadcast_no_waiter();
    test_cocond_concurrent_signal();

    // Run cancellation tests (scheduler is still running)
    test_cancel_source_basic();
    test_cancel_token_multiple();
    test_cancellation_in_coroutine();
    test_cancellation_ignore();

    // Run yield tests (scheduler is still running)
    test_yield_basic();
    test_yield_multiple_coroutines();

    // Run SafeTask tests (basic tests, don't need scheduler)
    test_safetask_basic();
    test_safetask_error();
    test_safetask_string();
    test_safetask_exception();
    test_safetask_unknown_exception();
    test_safetask_void_basic();
    test_safetask_void_error();
    test_safetask_void_exception();
    test_safetask_no_handle();
    test_safetask_not_completed();
    test_safetask_move_constructor();
    test_safetask_move_assignment();
    test_safetask_void_move();

    // Run SafeTask scheduler tests (scheduler is still running)
    test_safetask_co_await();
    test_safetask_co_await_error();
    test_safetask_scheduler_spawn();
    test_safetask_scheduler_void_spawn();

    // Run integration tests (Task 11)
    test_stress_mutex_contention();
    test_nested_coroutines();
    test_deeply_nested_coroutines();
    test_detached_coroutines();
    test_detached_coroutines_with_mutex();
    test_stress_mixed_operations();

    // Final shutdown
    coro::CoroutineScheduler::Instance().Shutdown();

    std::cout << "All tests PASSED!\n";
    return 0;
}