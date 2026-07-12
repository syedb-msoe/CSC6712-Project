cmake_minimum_required(VERSION 3.14)
project(btree_storage CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(btree_storage src/btree.cpp)
target_include_directories(btree_storage PUBLIC include)

add_library(btree_server src/btree_server.cpp src/wal.cpp)
target_link_libraries(btree_server PUBLIC btree_storage)
target_include_directories(btree_server PUBLIC include)

add_library(client src/client.cpp)
target_include_directories(client PUBLIC include)

add_executable(db_server src/server_main.cpp)
target_link_libraries(db_server btree_server)

add_executable(db_client src/client_main.cpp)
target_link_libraries(db_client client)

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

add_executable(server_test tests/server_test.cpp)
target_link_libraries(server_test btree_server client GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(btree_test)
gtest_discover_tests(server_test)

add_executable(bench_read_write benchmarks/bench_read_write.cpp)
target_link_libraries(bench_read_write btree_storage)
add_executable(bench_cache benchmarks/bench_cache.cpp)
target_link_libraries(bench_cache btree_storage)