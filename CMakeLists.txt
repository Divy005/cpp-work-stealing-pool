cmake_minimum_required(VERSION 3.16)
project(wsp_thread_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Warnings as errors: the project must compile cleanly under a strict toolchain.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# ---------------------------------------------------------------------------
# Optional sanitizer gate: -DWSP_SANITIZER=thread | address | undefined
# These are required gates for the project (zero data races, zero memory bugs).
# ---------------------------------------------------------------------------
set(WSP_SANITIZER "" CACHE STRING "Sanitizer: thread|address|undefined|address,undefined")
if(WSP_SANITIZER)
  message(STATUS "Building with sanitizer: ${WSP_SANITIZER}")
  add_compile_options(-fsanitize=${WSP_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${WSP_SANITIZER})
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(wsp
  src/global_queue_pool.cpp
  src/work_stealing_pool.cpp
)
target_include_directories(wsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wsp PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# GoogleTest (fetched; no system install required)
# ---------------------------------------------------------------------------
include(FetchContent)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# Fetched as a release tarball (not a git clone) so it works in restricted
# environments where only plain HTTPS is permitted. The hash pins the contents.
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz
  URL_HASH SHA256=81964fe578e9bd7c94dfdb09c8e4d6e6759e19967e397dbea48d1c10e45d0df2
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(wsp_tests
  tests/test_global_queue_pool.cpp
  tests/test_intrusive_deque.cpp
  tests/test_work_stealing_pool.cpp
)
target_link_libraries(wsp_tests PRIVATE wsp gtest_main)
add_test(NAME wsp_tests COMMAND wsp_tests)

# ---------------------------------------------------------------------------
# Benchmark (custom harness; google-benchmark optional / not required)
# ---------------------------------------------------------------------------
add_executable(wsp_bench bench/benchmark.cpp)
target_link_libraries(wsp_bench PRIVATE wsp)
