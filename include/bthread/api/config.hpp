// include/bthread/api/config.hpp
/**
 * @file config.hpp
 * @brief Configuration and lifecycle functions for the bthread library.
 *
 * This header provides functions to configure and manage the scheduler:
 * - worker_count(): Get the number of worker threads
 * - set_worker_count(): Set the number of worker threads
 * - init(): Initialize the scheduler
 * - shutdown(): Shutdown the scheduler
 */

#pragma once

#include "bthread/core/scheduler.hpp"

namespace bthread {

/**
 * @brief Get the number of worker threads.
 */
inline int worker_count() {
    return Scheduler::Instance().worker_count();
}

/**
 * @brief Set the number of worker threads.
 * Must be called before any tasks are spawned.
 */
inline void set_worker_count(int count) {
    Scheduler::Instance().set_worker_count(count);
}

/**
 * @brief Initialize the scheduler.
 * Automatically called on first spawn, but can be called explicitly.
 */
inline void init() {
    Scheduler::Instance().Init();
}

/**
 * @brief Shutdown the scheduler.
 * Waits for all workers to finish and cleans up resources.
 */
inline void shutdown() {
    Scheduler::Instance().Shutdown();
}

} // namespace bthread