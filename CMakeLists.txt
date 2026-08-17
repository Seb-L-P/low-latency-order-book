cmake_minimum_required(VERSION 3.16)
project(low_latency_order_book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
add_compile_options(-Wall -Wextra)

include_directories(include)

add_executable(run_tests
  tests/test_object_pool.cpp
  tests/test_price_level.cpp
  tests/test_order_book.cpp
  tests/test_differential.cpp
  tests/main.cpp
)

add_executable(benchmark
  benchmarks/benchmark_main.cpp
)

enable_testing()
add_test(NAME unit_tests COMMAND run_tests)
