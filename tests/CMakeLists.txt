cmake_minimum_required(VERSION 3.15)

# Simple test runner using C++ standard library
set(TEST_SOURCES
    task_group_test.cpp
    work_stealing_queue_test.cpp
    global_queue_test.cpp
    butex_test.cpp
    mutex_test.cpp
    cond_test.cpp
    timer_test.cpp
    timer_shard_test.cpp
    context_test.cpp
    scheduler_test.cpp
    wake_test.cpp
    debug_test.cpp
    multi_test.cpp
    bthread_test.cpp
    stress_test.cpp
    diag_mutex.cpp
    minimal_test.cpp
    mutex_bench_test.cpp
    mpsc_queue_test.cpp
    worker_batch_test.cpp
    xmm_test.cpp
)

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE bthread coro)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Coroutine test (requires C++20)
add_executable(coroutine_test coroutine_test.cpp)
target_link_libraries(coroutine_test PRIVATE coro)
add_test(NAME coroutine_test COMMAND coroutine_test)

# Stack pool test (requires gtest)
add_executable(stack_pool_test stack_pool_test.cpp)
target_link_libraries(stack_pool_test bthread gtest gtest_main)
add_test(NAME StackPoolTest COMMAND stack_pool_test)

# Task cache test (requires gtest)
add_executable(task_cache_test task_cache_test.cpp)
target_link_libraries(task_cache_test bthread gtest gtest_main)
add_test(NAME TaskCacheTest COMMAND task_cache_test)

# Idle registry test (requires gtest and pthread)
add_executable(idle_registry_test idle_registry_test.cpp)
target_link_libraries(idle_registry_test bthread gtest gtest_main pthread)
add_test(NAME IdleRegistryTest COMMAND idle_registry_test)

# Lazy Butex test (requires gtest and pthread)
add_executable(butex_lazy_test butex_lazy_test.cpp)
target_link_libraries(butex_lazy_test bthread gtest gtest_main pthread)
add_test(NAME ButexLazyTest COMMAND butex_lazy_test)

# Shared MPSC queue test (requires gtest and pthread)
add_executable(shared_mpsc_queue_test shared_mpsc_queue_test.cpp)
target_link_libraries(shared_mpsc_queue_test bthread gtest gtest_main pthread)
add_test(NAME SharedMPSCQueueTest COMMAND shared_mpsc_queue_test)

# Sync tests
add_subdirectory(sync)

# Performance tests (requires gtest and benchmark)
add_subdirectory(perf)