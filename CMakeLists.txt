cmake_minimum_required(VERSION 3.23)

project(sylar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

option(SYLAR_WITH_REDIS "Build the hiredis adapter and demo" OFF)
option(SYLAR_WITH_MYSQL "Build the MySQL adapter and demo" OFF)
option(SYLAR_WITH_ZOOKEEPER "Build the ZooKeeper adapter and demo" OFF)

add_subdirectory(sylar)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(SYLAR_BUILD_BENCHMARKS "Build standalone logging benchmarks" OFF)
if(SYLAR_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
