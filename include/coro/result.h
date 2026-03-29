// include/coro/result.h
// Type-safe error handling types for coroutine results
#pragma once

#include <string>
#include <variant>
#include <stdexcept>

namespace coro {

// Error class represents an error with code and message
class Error {
public:
    explicit Error(int code, std::string message)
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] int code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    int code_;
    std::string message_;
};

// Result<T> is a type-safe result type that can hold either a value or an error
template<typename T>
class Result {
public:
    // Success case
    explicit Result(T value) : data_(std::move(value)) {}

    // Error case
    explicit Result(Error error) : data_(std::move(error)) {}

    [[nodiscard]] bool is_ok() const noexcept { return data_.index() == 0; }
    [[nodiscard]] bool is_err() const noexcept { return data_.index() == 1; }

    T& value() {
        if (!is_ok()) {
            throw std::runtime_error("Result contains error");
        }
        return std::get<0>(data_);
    }

    const T& value() const {
        if (!is_ok()) {
            throw std::runtime_error("Result contains error");
        }
        return std::get<0>(data_);
    }

    Error& error() {
        if (!is_err()) {
            throw std::runtime_error("Result contains value");
        }
        return std::get<1>(data_);
    }

    const Error& error() const {
        if (!is_err()) {
            throw std::runtime_error("Result contains value");
        }
        return std::get<1>(data_);
    }

private:
    std::variant<T, Error> data_;
};

// Specialization for void
template<>
class Result<void> {
public:
    // Success case
    Result() : has_error_(false) {}

    // Error case
    explicit Result(Error error) : has_error_(true), error_(std::move(error)) {}

    [[nodiscard]] bool is_ok() const noexcept { return !has_error_; }
    [[nodiscard]] bool is_err() const noexcept { return has_error_; }

    Error& error() {
        if (!has_error_) {
            throw std::runtime_error("Result contains success");
        }
        return error_;
    }

    const Error& error() const {
        if (!has_error_) {
            throw std::runtime_error("Result contains success");
        }
        return error_;
    }

private:
    bool has_error_;
    Error error_{0, ""};
};

} // namespace coro