#pragma once

#include "bthread/queue/mpsc_queue.hpp"

namespace bthread {

/**
 * @brief Global queue for task distribution - alias for unified MPSC queue.
 *
 * Thread Safety:
 * - Push(): Safe to call from multiple producer threads concurrently.
 * - Pop(): Must only be called from a single consumer thread.
 *
 * Unified Design: Works with both TaskMeta (bthread) and CoroutineMeta (coroutine)
 * through the TaskMetaBase interface.
 */
using GlobalQueue = TaskQueue;

} // namespace bthread