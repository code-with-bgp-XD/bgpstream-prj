bgpstream_add_processor_plugin(
  timestamp_order_checker_plugin
  ${CMAKE_CURRENT_LIST_DIR}/timestamp_order_checker_plugin.cpp
)

target_compile_definitions(
  timestamp_order_checker_plugin
  PRIVATE
    BGPSTREAM_TIMESTAMP_LOG_DIR="${CMAKE_SOURCE_DIR}/log"
)
