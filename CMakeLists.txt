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
    set(PLATFORM_SOURCES src/platform/platform_linux.cpp src/platform/context_linux_x64.S)
    set(PLATFORM_HEADERS include/bthread/platform/platform_linux.h)
    set(PLATFORM_DEFINES BTHREAD_PLATFORM_LINUX)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# Library sources
set(BTHREAD_SOURCES
    src/bthread.cpp
    src/task_meta.cpp
    src/task_group.cpp
    src/work_stealing_queue.cpp
    src/global_queue.cpp
    src/worker.cpp
    src/scheduler.cpp
    src/butex.cpp
    src/mutex.cpp
    src/cond.cpp
    src/once.cpp
    src/timer_thread.cpp
    src/execution_queue.cpp
    src/platform/platform.cpp
    ${PLATFORM_SOURCES}
)

# Headers
set(BTHREAD_HEADERS
    include/bthread.h
    include/bthread/task_meta.h
    include/bthread/task_group.h
    include/bthread/work_stealing_queue.h
    include/bthread/global_queue.h
    include/bthread/worker.h
    include/bthread/scheduler.h
    include/bthread/butex.h
    include/bthread/mutex.h
    include/bthread/cond.h
    include/bthread/once.h
    include/bthread/timer_thread.h
    include/bthread/execution_queue.h
    include/bthread/platform/platform.h
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
    src/coro/mutex.cpp
    src/coro/cond.cpp
    src/coro/cancel.cpp
)

add_library(coro STATIC ${CORO_SOURCES})
target_include_directories(coro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Tests
enable_testing()
add_subdirectory(tests)

# Demo
add_subdirectory(demo)

# Benchmark
add_subdirectory(benchmark)