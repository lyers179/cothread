cmake_minimum_required(VERSION 3.15)
project(bthread VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform and compiler detection
if(WIN32)
    set(PLATFORM_HEADERS include/bthread/platform/platform_windows.h)
    set(PLATFORM_DEFINES BTHREAD_PLATFORM_WINDOWS)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)

    # Compiler-specific assembly file
    if(MSVC)
        enable_language(ASM_MASM)
        set(PLATFORM_SOURCES
            src/platform/platform_windows.cpp
            src/platform/context_windows_x64.asm
        )
        # Fix for MSVC standard library requiring EINVAL
        add_definitions(-DEINVAL=22)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        enable_language(ASM)
        set(PLATFORM_SOURCES
            src/platform/platform_windows.cpp
            src/platform/context_windows_x64_gcc.S
        )
        # GCC/Clang on Windows - no additional flags needed
    else()
        message(FATAL_ERROR "Unsupported compiler on Windows: ${CMAKE_CXX_COMPILER_ID}")
    endif()
elseif(UNIX AND NOT APPLE)
    enable_language(ASM)
    set(PLATFORM_SOURCES src/platform/platform_linux.cpp src/platform/context_linux_x64.S)
    set(PLATFORM_HEADERS include/bthread/platform/platform_linux.h)
    set(PLATFORM_DEFINES BTHREAD_PLATFORM_LINUX)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# Library sources
set(BTHREAD_SOURCES
    src/bthread.cpp
    src/bthread/core/task_meta.cpp
    src/bthread/core/task_group.cpp
    src/bthread/core/worker.cpp
    src/bthread/core/scheduler.cpp
    src/bthread/core/task.cpp
    src/bthread/core/coro_support.cpp
    src/bthread/queue/work_stealing_queue.cpp
    src/bthread/queue/execution_queue.cpp
    src/bthread/queue/shared_mpsc_queue.cpp
    src/bthread/sync/butex.cpp
    src/bthread/sync/mutex.cpp
    src/bthread/sync/cond.cpp
    src/bthread/sync/event.cpp
    src/bthread/api/once.cpp
    src/bthread/detail/timer_thread.cpp
    src/platform/platform.cpp
    ${PLATFORM_SOURCES}
)

# Headers
set(BTHREAD_HEADERS
    include/bthread.h
    include/bthread.hpp
    include/bthread/core/task_meta_base.hpp
    include/bthread/core/task_meta.hpp
    include/bthread/core/task_group.hpp
    include/bthread/core/worker.hpp
    include/bthread/core/scheduler.hpp
    include/bthread/core/task.hpp
    include/bthread/api.hpp
    include/bthread/queue/mpsc_queue.hpp
    include/bthread/queue/mpmc_queue.hpp
    include/bthread/queue/shared_mpsc_queue.hpp
    include/bthread/queue/global_queue.hpp
    include/bthread/queue/work_stealing_queue.hpp
    include/bthread/queue/execution_queue.hpp
    include/bthread/sync/butex.hpp
    include/bthread/sync/mutex.hpp
    include/bthread/sync/waiter.hpp
    include/bthread/sync/cond.hpp
    include/bthread/sync/event.hpp
    include/bthread/api/once.hpp
    include/bthread/detail/timer_thread.hpp
    include/bthread/platform/platform.h
    include/bthread/platform/platform.hpp
    include/bthread/platform/futex.hpp
    include/bthread/platform/stack.hpp
    include/bthread/platform/context.hpp
    ${PLATFORM_HEADERS}
)

# Static library
add_library(bthread STATIC ${BTHREAD_SOURCES} ${BTHREAD_HEADERS})
target_include_directories(bthread PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(bthread PUBLIC ${PLATFORM_DEFINES})

# Link libraries
if(WIN32)
    target_link_libraries(bthread PRIVATE synchronization)
else()
    target_link_libraries(bthread PRIVATE pthread)
endif()

# Coro library - C++20 coroutine support
set(CORO_SOURCES
    src/coro/frame_pool.cpp
    src/coro/coroutine.cpp
    src/coro/scheduler.cpp
    src/coro/cancel.cpp
)

add_library(coro STATIC ${CORO_SOURCES})
target_include_directories(coro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(coro PUBLIC bthread)

# Tests
enable_testing()
add_subdirectory(tests)

# Demo
add_subdirectory(demo)

# Benchmark
add_subdirectory(benchmark)