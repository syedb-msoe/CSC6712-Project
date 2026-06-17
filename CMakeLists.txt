cmake_minimum_required(VERSION 3.14)
project(btree_storage CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(btree_storage src/btree.cpp)
target_include_directories(btree_storage PUBLIC include)

# Tests
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(btree_test tests/btree_test.cpp)
target_link_libraries(btree_test btree_storage GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(btree_test)