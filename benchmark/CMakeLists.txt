cmake_minimum_required(VERSION 3.15)

add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark PRIVATE bthread coro)

# Fix for MSVC standard library requiring EINVAL
if(WIN32)
    target_compile_definitions(benchmark PRIVATE EINVAL=22)
endif()