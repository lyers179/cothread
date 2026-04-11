cmake_minimum_required(VERSION 3.15)

# MPMC Queue test (requires pthread)
add_executable(mpmc_queue_test mpmc_queue_test.cpp)
target_link_libraries(mpmc_queue_test PRIVATE bthread pthread)
add_test(NAME MpmcQueueTest COMMAND mpmc_queue_test)