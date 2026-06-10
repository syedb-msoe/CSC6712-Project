cmake_minimum_required(VERSION 3.14)
project(btree_storage CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(btree_storage src/btree.cpp)
target_include_directories(btree_storage PUBLIC include)
