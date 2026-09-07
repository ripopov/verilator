# DESCRIPTION: Verilator: CMake build for verilator_vdb_index, the slang-based
# source index exporter appended to VDB companions of --trace-vtr models.
#
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

cmake_minimum_required(VERSION 3.20)
project(verilator_vdb_index LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# slang is pinned as a submodule of the Verilator tree (ext/slang) so the
# exporter always elaborates with the frontend version it was tested against.
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "" FORCE)
set(SLANG_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../ext/slang ${CMAKE_CURRENT_BINARY_DIR}/slang EXCLUDE_FROM_ALL)

add_library(vdb_index_lib STATIC Json.cpp SourceIndex.cpp)
target_include_directories(vdb_index_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vdb_index_lib PUBLIC slang::slang)
target_compile_options(vdb_index_lib PRIVATE -Wall -Wextra)

add_executable(verilator_vdb_index main.cpp)
target_link_libraries(verilator_vdb_index PRIVATE vdb_index_lib)

enable_testing()
add_executable(vdb_index_test tests/IndexTests.cpp)
target_link_libraries(vdb_index_test PRIVATE vdb_index_lib)
target_compile_definitions(vdb_index_test PRIVATE
  VDB_INDEX_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
add_test(NAME vdb_index_test COMMAND vdb_index_test)
