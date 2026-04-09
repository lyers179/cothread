cmake_minimum_required(VERSION 3.15)

# MPSC performance test (gtest only)
add_executable(mpsc_perf_test mpsc_perf_test.cpp)
target_link_libraries(mpsc_perf_test PRIVATE bthread gtest gtest_main)
add_test(NAME MpscPerfTest COMMAND mpsc_perf_test)