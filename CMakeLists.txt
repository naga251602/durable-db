cmake_minimum_required(VERSION 3.16)
project(durable_db CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(BUILD_TESTS "Build unit tests (downloads GoogleTest)" ON)

add_compile_options(-Wall -Wextra -Wpedantic)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(durabledb
  src/db.cpp src/wal.cpp src/memtable.cpp src/sstable.cpp)
target_include_directories(durabledb PUBLIC include)

# Small command line tool: durabledb_cli <dir> put|get|del|stats ...
add_executable(durabledb_cli tools/durabledb_cli.cpp)
target_link_libraries(durabledb_cli durabledb)

# Crash test helpers (driven by tools/crash_test.sh)
add_executable(crash_writer tools/crash_writer.cpp)
target_link_libraries(crash_writer durabledb)
add_executable(crash_verify tools/crash_verify.cpp)
target_link_libraries(crash_verify durabledb)

# Benchmarks
add_executable(bench bench/bench_main.cpp)
target_link_libraries(bench durabledb)

if(BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  file(GLOB TEST_SRCS CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(unit_tests ${TEST_SRCS})
  target_link_libraries(unit_tests durabledb GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(unit_tests)
endif()
