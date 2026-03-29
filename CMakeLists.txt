cmake_minimum_required(VERSION 3.16)

project(bgpstream_runner LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are disabled. Use: cmake -S . -B build")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

foreach(config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${config}" config_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}")
endforeach()

find_package(Curses REQUIRED)
find_package(Threads REQUIRED)
find_path(BGPSTREAM_INCLUDE_DIR NAMES bgpstream.h REQUIRED)
find_library(BGPSTREAM_LIBRARY NAMES bgpstream REQUIRED)

add_executable(
  bgpstream_prefix_stats
  cpp/src/main.cpp
  cpp/src/common.cpp
  cpp/src/config_file.cpp
  cpp/src/download_client.cpp
  cpp/src/chunk_engine.cpp
  cpp/src/prefix_as_stats_processor.cpp
)

target_include_directories(
  bgpstream_prefix_stats
  PRIVATE
    cpp/include
    ${BGPSTREAM_INCLUDE_DIR}
    ${CURSES_INCLUDE_DIRS}
)

target_compile_definitions(
  bgpstream_prefix_stats
  PRIVATE
    BGPSTREAM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    BGPSTREAM_DOWNLOAD_SCRIPT_PATH="${CMAKE_SOURCE_DIR}/python/download.py"
)

target_link_libraries(
  bgpstream_prefix_stats
  PRIVATE
    ${BGPSTREAM_LIBRARY}
    ${CURSES_LIBRARIES}
    Threads::Threads
)
