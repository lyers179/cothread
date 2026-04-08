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
// On Windows, we need to provide these since they may not be available
// in all contexts. Use macros to ensure MSVC standard library can find them.
#ifdef _WIN32
#ifndef EPERM
#define EPERM 1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ESRCH
#define ESRCH 3
#endif
#ifndef EINTR
#define EINTR 4
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef ENXIO
#define ENXIO 6
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EFAULT
#define EFAULT 14
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif
#ifndef ENOSYS
#define ENOSYS 38
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#endif

// Namespace constants for type-safe usage (values match macros above)
inline constexpr int EPERM_VAL = EPERM;
inline constexpr int ENOENT_VAL = ENOENT;
inline constexpr int ESRCH_VAL = ESRCH;
inline constexpr int EINTR_VAL = EINTR;
inline constexpr int EIO_VAL = EIO;
inline constexpr int ENXIO_VAL = ENXIO;
inline constexpr int EAGAIN_VAL = EAGAIN;
inline constexpr int ENOMEM_VAL = ENOMEM;
inline constexpr int EACCES_VAL = EACCES;
inline constexpr int EFAULT_VAL = EFAULT;
inline constexpr int EBUSY_VAL = EBUSY;
inline constexpr int EEXIST_VAL = EEXIST;
inline constexpr int ENODEV_VAL = ENODEV;
inline constexpr int EINVAL_VAL = EINVAL;
inline constexpr int ENOTTY_VAL = ENOTTY;
inline constexpr int EDEADLK_VAL = EDEADLK;
inline constexpr int ENAMETOOLONG_VAL = ENAMETOOLONG;
inline constexpr int ENOSYS_VAL = ENOSYS;
inline constexpr int ETIMEDOUT_VAL = ETIMEDOUT;

// ============ Futex Operations ============

// timespec struct - only defined on Windows where it doesn't exist in system headers
#ifdef _WIN32
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
#else
// On Linux/Unix, use the system's timespec from <time.h>
#include <time.h>
// Type alias for consistency with Windows code
using timespec = ::timespec;
#endif

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