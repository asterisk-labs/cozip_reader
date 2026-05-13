cmake_minimum_required(VERSION 3.5)

# cozip is an out-of-tree community extension. The build is driven by the
# DuckDB extension template, which provides build_static_extension,
# build_loadable_extension and DUCKDB_EXPORT_SET. The parquet extension is
# built in-tree, so its sources live under the duckdb
# submodule and we can link against the parquet_extension CMake target.

set(TARGET_NAME cozip)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Local includes plus the parquet extension's public headers and the third
# party headers it transitively exposes. We need them because we instantiate
# ParquetReader directly in C++ and its public header pulls in thrift,
# snappy, zstd, mbedtls and lz4 declarations. Without these include paths the
# parquet_reader.hpp include chain fails to resolve.
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

# Every cpp file that defines symbols for the cozip extension belongs here.
# Forgetting cozip_subfile_fs.cpp would compile fine because the header
# declares the class and the cozip-subfile virtual filesystem is only used
# through that header in cozip_extension.cpp, but the link step would fail
# with undefined references to CozipSubFileSystem::OpenFile, ::Read, etc.
set(EXTENSION_SOURCES
    src/cozip_extension.cpp
    src/cozip_subfile_fs.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link against the parquet extension target so we can call ParquetReader
# directly from C++. parquet_extension is the static-build target; the
# loadable build target is parquet_loadable_extension but for linking from a
# downstream extension we want the static archive on both our targets.
target_link_libraries(${EXTENSION_NAME} parquet_extension)
target_link_libraries(${LOADABLE_EXTENSION_NAME} parquet_extension)

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)