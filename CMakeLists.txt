cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME cozip)

# DuckDB extension distribution supports vcpkg. cozip currently has no
# third-party dependencies. It does statically link the core `parquet`
# extension so we can call ParquetReader directly from C++.

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Local includes + parquet extension's public headers. The parquet extension
# is built in-tree because we declare `duckdb_extension_load(parquet)` in
# `extension_config.cmake`, so its include tree lives under the duckdb
# submodule.
include_directories(
  src/include
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/extension/parquet/include
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/parquet
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/thrift
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/snappy
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/zstd/include
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/mbedtls/include
  ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/lz4
)

set(EXTENSION_SOURCES src/cozip_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link against the parquet extension so we can call its public API directly from C++.
target_link_libraries(${EXTENSION_NAME} parquet_extension)
target_link_libraries(${LOADABLE_EXTENSION_NAME} parquet_extension)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")