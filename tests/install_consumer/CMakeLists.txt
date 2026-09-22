cmake_minimum_required (VERSION 3.25)
project (ge_install_consumer LANGUAGES C CXX)

find_package (ge CONFIG REQUIRED)

add_library (ge_install_plugin MODULE install_plugin.c)
target_link_libraries (ge_install_plugin PRIVATE ge::ge_headers)
set_target_properties (ge_install_plugin PROPERTIES PREFIX "")

add_executable (ge_install_cpp_consumer install_cpp_sample.cpp)
target_link_libraries (ge_install_cpp_consumer PRIVATE ge::ge_core)
