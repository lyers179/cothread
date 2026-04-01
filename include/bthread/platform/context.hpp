// include/bthread/platform/context.hpp
/**
 * @file context.hpp
 * @brief Context switching for bthreads.
 *
 * Provides platform-specific context switching operations used by
 * bthreads for user-space context switching.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace bthread {
namespace platform {

/**
 * @brief Context structure for user-space context switching.
 *
 * Platform-dependent storage for CPU registers and stack pointer.
 * Used to save/restore execution context during bthread switches.
 */
struct Context {
    // General purpose registers (platform-specific layout)
    union {
        uint64_t gp_regs[16];     ///< General purpose registers
        void* ptr_regs[16];       ///< Alternative pointer view
    };

    // XMM registers for Windows x64 (10 x 16 bytes = 160 bytes)
    alignas(16) uint8_t xmm_regs[160];

    // Stack pointer
    void* stack_ptr;

    // Return address (for wrapper)
    void* return_addr;
};

/// Thread handle type
using ThreadId = void*;

/// Thread entry point type
using ThreadFunc = void(*)(void*);

// ============ Context Switching ============

/**
 * @brief Make a new context ready to run.
 *
 * Initializes a context structure to run `fn` with argument `arg`
 * when resumed. The context will use the provided stack.
 *
 * @param ctx Context to initialize
 * @param stack Stack memory
 * @param stack_size Stack size
 * @param fn Entry function
 * @param arg Argument to pass to entry function
 */
extern "C" void MakeContext(Context* ctx, void* stack, size_t stack_size,
                            ThreadFunc fn, void* arg);

/**
 * @brief Swap contexts.
 *
 * Saves current context to `from` and switches to `to`.
 * When resumed, returns to the caller.
 *
 * @param from Location to save current context
 * @param to Context to switch to
 */
extern "C" void SwapContext(Context* from, Context* to);

// ============ Thread Management ============

/**
 * @brief Create a new thread.
 * @param fn Entry function
 * @param arg Argument to pass
 * @return Thread handle
 */
ThreadId CreateThread(ThreadFunc fn, void* arg);

/**
 * @brief Join a thread.
 * @param thread Thread to join
 */
void JoinThread(ThreadId thread);

} // namespace platform
} // namespace bthread