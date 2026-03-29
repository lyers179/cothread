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

// Context switching functions implemented in assembly - use C linkage
extern "C" {

// Make a new context ready to run 'fn' with 'arg'
void MakeContext(Context* ctx, void* stack, size_t stack_size, ThreadFunc fn, void* arg);

// Swap contexts: saves current context to 'from', loads 'to'
void SwapContext(Context* from, Context* to);

} // extern "C"

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

// ============ Error Codes ============
// Common error codes (matches errno values)
// Undef Windows macros before defining our own
#ifdef _WIN32
#undef EPERM
#undef ENOENT
#undef ESRCH
#undef EINTR
#undef EIO
#undef ENXIO
#undef EAGAIN
#undef ENOMEM
#undef EACCES
#undef EFAULT
#undef EBUSY
#undef EEXIST
#undef ENODEV
#undef EINVAL
#undef ENOTTY
#undef EDEADLK
#undef ENAMETOOLONG
#undef ENOSYS
#undef ETIMEDOUT
#endif

inline constexpr int EPERM = 1;
inline constexpr int ENOENT = 2;
inline constexpr int ESRCH = 3;
inline constexpr int EINTR = 4;
inline constexpr int EIO = 5;
inline constexpr int ENXIO = 6;
inline constexpr int EAGAIN = 11;
inline constexpr int ENOMEM = 12;
inline constexpr int EACCES = 13;
inline constexpr int EFAULT = 14;
inline constexpr int EBUSY = 16;
inline constexpr int EEXIST = 17;
inline constexpr int ENODEV = 19;
inline constexpr int EINVAL = 22;
inline constexpr int ENOTTY = 25;
inline constexpr int EDEADLK = 35;
inline constexpr int ENAMETOOLONG = 36;
inline constexpr int ENOSYS = 38;
inline constexpr int ETIMEDOUT = 110;

// ============ Futex Operations ============

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

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