// include/bthread/platform/platform.hpp
/**
 * @file platform.hpp
 * @brief Platform detection and configuration macros.
 *
 * This header provides platform detection macros and common definitions
 * used throughout the bthread library.
 */

#pragma once

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
    #define BTHREAD_PLATFORM_WINDOWS 1
    #define BTHREAD_PLATFORM_LINUX 0
#elif defined(__linux__)
    #define BTHREAD_PLATFORM_WINDOWS 0
    #define BTHREAD_PLATFORM_LINUX 1
#else
    #error "Unsupported platform"
#endif

// Architecture detection
#if defined(_M_X64) || defined(__x86_64__)
    #define BTHREAD_ARCH_X64 1
#elif defined(_M_IX86) || defined(__i386__)
    #define BTHREAD_ARCH_X86 1
#elif defined(_M_ARM64) || defined(__aarch64__)
    #define BTHREAD_ARCH_ARM64 1
#else
    #define BTHREAD_ARCH_UNKNOWN 1
#endif

// Compiler detection
#if defined(_MSC_VER)
    #define BTHREAD_COMPILER_MSVC 1
    #define BTHREAD_COMPILER_GCC 0
    #define BTHREAD_COMPILER_CLANG 0
#elif defined(__GNUC__)
    #define BTHREAD_COMPILER_MSVC 0
    #define BTHREAD_COMPILER_GCC 1
    #define BTHREAD_COMPILER_CLANG 0
#elif defined(__clang__)
    #define BTHREAD_COMPILER_MSVC 0
    #define BTHREAD_COMPILER_GCC 0
    #define BTHREAD_COMPILER_CLANG 1
#endif

// Common platform definitions
namespace bthread {
namespace platform {

// Page size for stack guard
constexpr size_t PAGE_SIZE = 4096;

// Default stack size for bthreads
constexpr size_t DEFAULT_STACK_SIZE = 1024 * 1024;  // 1MB

// Maximum worker threads
constexpr int MAX_WORKERS = 256;

// Task pool size
constexpr size_t TASK_POOL_SIZE = 16384;

} // namespace platform
} // namespace bthread