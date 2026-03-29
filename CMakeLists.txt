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

add_library(
  bgpstream_runner_core
  STATIC
  cpp/src/common.cpp
  cpp/src/config_file.cpp
  cpp/src/download_client.cpp
  cpp/src/chunk_engine.cpp
)

add_library(bgpstream_runner_sdk INTERFACE)
target_include_directories(
  bgpstream_runner_sdk
  INTERFACE
    cpp/include
)
target_compile_features(bgpstream_runner_sdk INTERFACE cxx_std_17)

target_include_directories(
  bgpstream_runner_core
  PRIVATE
    cpp/include
    ${BGPSTREAM_INCLUDE_DIR}
    ${CURSES_INCLUDE_DIRS}
)

target_compile_definitions(
  bgpstream_runner_core
  PRIVATE
    BGPSTREAM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    BGPSTREAM_DOWNLOAD_SCRIPT_PATH="${CMAKE_SOURCE_DIR}/python/download.py"
)

target_link_libraries(
  bgpstream_runner_core
  PUBLIC
    bgpstream_runner_sdk
  PRIVATE
    ${BGPSTREAM_LIBRARY}
    ${CURSES_LIBRARIES}
    Threads::Threads
)

function(bgpstream_add_processor_plugin target_name)
  add_library(${target_name} MODULE ${ARGN})
  set_target_properties(${target_name} PROPERTIES PREFIX "")
  target_link_libraries(${target_name} PRIVATE bgpstream_runner_sdk)
  set_property(GLOBAL APPEND PROPERTY BGPSTREAM_PROCESSOR_PLUGIN_TARGETS ${target_name})
endfunction()

add_executable(
  bgpstream_prefix_stats
  cpp/src/main.cpp
  cpp/src/plugin_loader.cpp
)

target_include_directories(
  bgpstream_prefix_stats
  PRIVATE
    cpp/include
)

target_link_libraries(
  bgpstream_prefix_stats
  PRIVATE
    bgpstream_runner_core
    ${CMAKE_DL_LIBS}
)

add_subdirectory(examples)
add_subdirectory(plugins)

get_property(BGPSTREAM_PROCESSOR_PLUGIN_TARGETS GLOBAL PROPERTY BGPSTREAM_PROCESSOR_PLUGIN_TARGETS)
set(BGPSTREAM_PROCESSOR_PLUGIN_MANIFEST_CONTENT "")
foreach(plugin_target IN LISTS BGPSTREAM_PROCESSOR_PLUGIN_TARGETS)
  string(APPEND BGPSTREAM_PROCESSOR_PLUGIN_MANIFEST_CONTENT
         "${plugin_target}\t$<TARGET_FILE:${plugin_target}>\n")
endforeach()
file(GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/bgpstream_processor_plugins.tsv"
  CONTENT "${BGPSTREAM_PROCESSOR_PLUGIN_MANIFEST_CONTENT}"
)
