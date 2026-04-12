cmake_minimum_required(VERSION 3.15)

add_executable(object_pool_test object_pool_test.cpp)
target_link_libraries(object_pool_test PRIVATE bthread gtest gtest_main pthread)
add_test(NAME ObjectPoolTest COMMAND object_pool_test)