// tests/coroutine_test.cpp
#include "coro/result.h"
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

int main() {
    test_error_basic();
    test_result_value();
    test_result_error();
    test_result_void();
    test_result_value_throws_on_error();
    test_result_error_throws_on_success();
    test_result_void_error_throws_on_success();
    test_result_string_move_semantics();

    std::cout << "All tests PASSED!\n";
    return 0;
}