// tests/coroutine_test.cpp
#include "coro/result.h"
#include "coro/frame_pool.h"
#include <cassert>
#include <iostream>

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

    std::cout << "All tests PASSED!\n";
    return 0;
}