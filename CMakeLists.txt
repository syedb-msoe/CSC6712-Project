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

add_library(murmur third_party/murmur/MurmurHash3.cpp)
target_include_directories(murmur PUBLIC third_party/murmur)

add_library(dist_client src/dist_client.cpp)
target_link_libraries(dist_client PUBLIC client murmur)
target_include_directories(dist_client PUBLIC include)

add_executable(db_server src/server_main.cpp)
target_link_libraries(db_server btree_server)

add_executable(db_client src/client_main.cpp)
target_link_libraries(db_client client)

add_executable(dist_db_client src/dist_client_main.cpp)
target_link_libraries(dist_db_client dist_client)

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

add_executable(rendezvous_test tests/rendezvous_test.cpp)
target_link_libraries(rendezvous_test murmur GTest::gtest_main)
target_include_directories(rendezvous_test PRIVATE include)

add_executable(dist_client_test tests/dist_client_test.cpp)
target_link_libraries(dist_client_test btree_server dist_client GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(btree_test)
gtest_discover_tests(server_test)
gtest_discover_tests(rendezvous_test)
gtest_discover_tests(dist_client_test)

add_executable(bench_read_write benchmarks/bench_read_write.cpp)
target_link_libraries(bench_read_write btree_storage)
add_executable(bench_cache benchmarks/bench_cache.cpp)
target_link_libraries(bench_cache btree_storage)

find_package(Threads REQUIRED)
add_executable(bench_load benchmarks/bench_load.cpp)
target_link_libraries(bench_load btree_storage)
add_executable(bench_connections benchmarks/bench_connections.cpp)
target_link_libraries(bench_connections client Threads::Threads)
add_executable(bench_distributed benchmarks/bench_distributed.cpp)
target_link_libraries(bench_distributed dist_client Threads::Threads)