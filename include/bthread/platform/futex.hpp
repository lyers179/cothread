// include/bthread/platform/futex.hpp
/**
 * @file futex.hpp
 * @brief Futex operations for efficient waiting.
 *
 * Provides platform-abstracted futex operations:
 * - Linux: Uses native futex syscalls
 * - Windows: Uses WaitOnAddress/WakeByAddress
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace bthread {
namespace platform {

// Platform-specific timespec
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/**
 * @brief Wait on an address (futex).
 *
 * Blocks the calling thread until the value at `addr` changes
 * from `expected`, or until timeout expires.
 *
 * @param addr Address to wait on
 * @param expected Expected value
 * @param timeout Timeout (nullptr for infinite wait)
 * @return 0 on success, error code on failure/timeout
 */
int FutexWait(std::atomic<int>* addr, int expected, const timespec* timeout);

/**
 * @brief Wake waiters on an address (futex).
 *
 * Wakes up to `count` threads waiting on `addr`.
 *
 * @param addr Address to wake on
 * @param count Maximum number of waiters to wake
 * @return Number of waiters woken
 */
int FutexWake(std::atomic<int>* addr, int count);

} // namespace platform
} // namespace bthread