cmake_minimum_required(VERSION 3.24)

project(wl2_resource_conflict_test LANGUAGES CXX)

include("${WL2_RESOURCES_MODULE}")

add_executable(conflict main.cpp)
wl2_add_embedded_resources(conflict
    ROOT ${CMAKE_CURRENT_SOURCE_DIR}/files
    PREFIX wl2:/conflict
    STORE_FILES dup.txt
    COMPRESS_FILES dup.txt)
