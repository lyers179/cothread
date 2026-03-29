// tests/coroutine_test.cpp
#include "coro/result.h"
#include "coro/frame_pool.h"
#include "coro/meta.h"
#include "coro/coroutine.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>

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
    assert(meta.state.load() == coro::CoroutineMeta::READY);
    assert(meta.owner_worker == nullptr);
    assert(meta.cancel_requested.load() == false);
    assert(meta.waiting_sync == nullptr);
    assert(meta.next.load() == nullptr);
    assert(meta.slot_index == 0);
    assert(meta.generation == 0);

    // Test state transitions (atomic operations)
    meta.state.store(coro::CoroutineMeta::RUNNING);
    assert(meta.state.load() == coro::CoroutineMeta::RUNNING);

    meta.state.store(coro::CoroutineMeta::SUSPENDED);
    assert(meta.state.load() == coro::CoroutineMeta::SUSPENDED);

    meta.state.store(coro::CoroutineMeta::FINISHED);
    assert(meta.state.load() == coro::CoroutineMeta::FINISHED);

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

int main() {
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

    std::cout << "All tests PASSED!\n";
    return 0;
}