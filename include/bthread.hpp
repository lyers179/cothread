// include/bthread.hpp
/**
 * @file bthread.hpp
 * @brief Modern C++ API for the bthread M:N threading library.
 *
 * This is the main convenience header that includes all the modern C++ API
 * for both bthread (assembly-based context switching) and coroutine (C++20)
 * execution models.
 *
 * Usage:
 * ```cpp
 * #include <bthread.hpp>
 *
 * // Spawn a bthread
 * auto task = bthread::spawn([]{
 *     std::cout << "Hello from bthread!" << std::endl;
 * });
 * task.join();
 *
 * // Spawn a coroutine
 * auto coro_task = bthread::spawn_coro([]() -> coro::Task<int> {
 *     co_await coro::sleep(std::chrono::milliseconds(100));
 *     co_return 42;
 * });
 * ```
 */

#pragma once

// Core
#include "bthread/core/task_meta_base.hpp"
#include "bthread/core/task.hpp"
#include "bthread/core/scheduler.hpp"

// Queue
#include "bthread/queue/mpsc_queue.hpp"

// Sync
#include "bthread/sync/mutex.hpp"
#include "bthread/sync/cond.hpp"
#include "bthread/sync/event.hpp"

// API
#include "bthread/api/spawn.hpp"
#include "bthread/api/config.hpp"

// Coroutine support
#include "coro/coroutine.h"
#include "coro/meta.h"

namespace bthread {

/**
 * @brief Main namespace for the bthread library.
 *
 * Provides:
 * - Task-based API for both bthread and coroutine execution
 * - Unified synchronization primitives (Mutex, CondVar, Event)
 * - Integration with C++20 coroutines
 *
 * Quick Start:
 * ```cpp
 * #include <bthread.hpp>
 *
 * int main() {
 *     // Initialize with 4 worker threads
 *     bthread::set_worker_count(4);
 *     bthread::init();
 *
 *     // Spawn a task
 *     auto task = bthread::spawn([]{
 *         return 42;
 *     });
 *
 *     // Wait for completion
 *     task.join();
 *
 *     // Cleanup
 *     bthread::shutdown();
 *     return 0;
 * }
 * ```
 */

} // namespace bthread