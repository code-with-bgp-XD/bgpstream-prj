bgpstream_add_processor_plugin(
  example_message_summary_plugin
  ${CMAKE_CURRENT_LIST_DIR}/example_message_summary_plugin.cpp
)

bgpstream_add_processor_plugin(
  example_announcement_counter_plugin
  ${CMAKE_CURRENT_LIST_DIR}/example_announcement_counter_plugin.cpp
)

bgpstream_add_processor_plugin(
  example_withdrawal_prefix_plugin
  ${CMAKE_CURRENT_LIST_DIR}/example_withdrawal_prefix_plugin.cpp
)

bgpstream_add_processor_plugin(
  example_origin_asn_plugin
  ${CMAKE_CURRENT_LIST_DIR}/example_origin_asn_plugin.cpp
)
