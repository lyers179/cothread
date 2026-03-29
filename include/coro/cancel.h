// include/coro/cancel.h
#pragma once

#include <atomic>
#include <coroutine>
#include "coro/meta.h"

namespace coro {

/**
 * @brief Token to check if cancellation has been requested.
 *
 * CancellationToken is obtained from a CancelSource and can be passed to
 * coroutines to enable cooperative cancellation. The token provides a way
 * to check if cancellation has been requested via is_cancelled() or
 * co_await check_cancel().
 *
 * @warning LIFETIME REQUIREMENT: The CancellationToken MUST NOT outlive the
 * CancelSource that created it. The token stores a raw pointer to the source's
 * internal atomic flag. If the source is destroyed before the token, accessing
 * the token results in undefined behavior (dangling pointer dereference).
 *
 * Typical usage: The CancelSource should have a longer or equal lifetime to
 * all tokens created from it. For example, store the CancelSource as a member
 * variable and pass tokens to child operations that complete before the parent
 * is destroyed.
 *
 * Thread Safety: All operations are thread-safe.
 */
class CancellationToken {
public:
    explicit CancellationToken(std::atomic<bool>& flag) : flag_(&flag) {}

    /**
     * @brief Check if cancellation has been requested.
     * Thread-safe atomic load.
     */
    bool is_cancelled() const {
        return flag_->load(std::memory_order_acquire);
    }

    /**
     * @brief Awaitable for checking cancellation in a coroutine.
     * Returns true if cancellation was requested, false otherwise.
     * This is a non-suspending awaitable (await_ready() returns true).
     */
    class CheckCancelAwaiter {
    public:
        explicit CheckCancelAwaiter(std::atomic<bool>& flag) : flag_(flag) {}

        // Always ready - this is a non-blocking check
        bool await_ready() noexcept { return true; }

        // Never suspends
        void await_suspend(std::coroutine_handle<>) noexcept {}

        // Returns the cancellation state
        bool await_resume() noexcept {
            return flag_.load(std::memory_order_acquire);
        }

    private:
        std::atomic<bool>& flag_;
    };

    /**
     * @brief Get an awaitable to check cancellation state.
     * Usage: bool cancelled = co_await token.check_cancel();
     */
    CheckCancelAwaiter check_cancel() {
        return CheckCancelAwaiter(*flag_);
    }

private:
    std::atomic<bool>* flag_;
};

/**
 * @brief Source for requesting cancellation.
 *
 * CancelSource creates CancellationToken objects that share the same
 * cancellation state. Calling cancel() signals all associated tokens.
 *
 * Thread Safety: All operations are thread-safe.
 */
class CancelSource {
public:
    CancelSource() : cancelled_(false) {}

    /**
     * @brief Get a cancellation token associated with this source.
     *
     * Multiple calls return tokens sharing the same cancellation state.
     *
     * @warning LIFETIME: The returned token holds a pointer to this CancelSource's
     * internal state. The returned token MUST NOT outlive this CancelSource.
     * Destroying the CancelSource while a token is still in use causes undefined
     * behavior when the token is accessed.
     *
     * @return CancellationToken associated with this source.
     */
    CancellationToken token() {
        return CancellationToken(cancelled_);
    }

    /**
     * @brief Request cancellation.
     * After this call, all associated tokens will report is_cancelled() == true.
     * Thread-safe atomic store.
     */
    void cancel() {
        cancelled_.store(true, std::memory_order_release);
    }

    /**
     * @brief Check if cancellation has been requested.
     */
    bool is_cancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }

    /**
     * @brief Reset the cancellation state to false.
     * Allows reusing the CancelSource for a new operation.
     */
    void reset() {
        cancelled_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> cancelled_;
};

} // namespace coro