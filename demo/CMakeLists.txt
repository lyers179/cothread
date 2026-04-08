cmake_minimum_required(VERSION 3.15)

add_executable(demo demo.cpp)
target_link_libraries(demo PRIVATE bthread coro)

add_executable(coro_demo coro_demo.cpp)
target_link_libraries(coro_demo PRIVATE coro bthread)

add_executable(yield_demo yield_demo.cpp)
target_link_libraries(yield_demo PRIVATE bthread coro)