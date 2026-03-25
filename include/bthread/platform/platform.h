#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>

namespace bthread {
namespace platform {

// Context structure for user-space context switching
struct Context {
    union {
        uint64_t gp_regs[16];     // General purpose registers
        void* ptr_regs[16];
    };

    // XMM registers for Windows (10 x 16 bytes = 160 bytes)
    alignas(16) uint8_t xmm_regs[160];

    // Stack pointer
    void* stack_ptr;

    // Return address (for wrapper)
    void* return_addr;
};

// Thread handle type
using ThreadId = void*;

// Thread entry point type
using ThreadFunc = void(*)(void*);

// Page size for stack guard
constexpr size_t PAGE_SIZE = 4096;

// ============ Context Switching ============

// Make a new context ready to run 'fn' with 'arg'
void MakeContext(Context* ctx, void* stack, size_t stack_size, ThreadFunc fn, void* arg);

// Swap contexts: saves current context to 'from', loads 'to'
void SwapContext(Context* from, Context* to);

// ============ Thread Management ============

// Create a new thread
ThreadId CreateThread(ThreadFunc fn, void* arg);

// Join a thread
void JoinThread(ThreadId thread);

// ============ Stack Allocation ============

// Allocate a stack with guard page
void* AllocateStack(size_t size);

// Deallocate a stack
void DeallocateStack(void* stack, size_t size);

// ============ Futex Operations ============

struct timespec;

// Wait on address (Linux: futex, Windows: WaitOnAddress)
int FutexWait(std::atomic<int>* addr, int expected, const timespec* timeout);

// Wake waiters on address
int FutexWake(std::atomic<int>* addr, int count);

// ============ Time Utilities ============

// Get current time in microseconds since epoch
int64_t GetTimeOfDayUs();

// ============ Stack Overflow Handler ============

// Set up stack overflow handler (called during scheduler init)
void SetupStackOverflowHandler();

} // namespace platform
} // namespace bthread