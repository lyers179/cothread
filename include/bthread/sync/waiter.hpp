#pragma once

#include <atomic>
#include <cstdint>

namespace bthread {

/**
 * @brief Unified wait state for synchronization primitives.
 *
 * This struct contains the state needed for threads/coroutines waiting
 * on synchronization primitives like butex, mutex, cond, etc.
 *
 * Note: TaskMeta already has a WaiterState member which includes
 * additional fields for queue management (next/prev pointers).
 */
struct WaitState {
    std::atomic<bool> is_waiting{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> wakeup{false};
    int64_t deadline_us{0};
    int timer_id{0};
};

} // namespace bthread