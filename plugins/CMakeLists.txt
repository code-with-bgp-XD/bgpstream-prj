file(GLOB BGPSTREAM_PLUGIN_CMAKE_LISTS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "*/CMakeLists.txt")

foreach(plugin_cmake IN LISTS BGPSTREAM_PLUGIN_CMAKE_LISTS)
  get_filename_component(plugin_dir "${plugin_cmake}" DIRECTORY)
  add_subdirectory("${plugin_dir}")
endforeach()
