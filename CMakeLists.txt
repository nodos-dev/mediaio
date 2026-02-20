# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

target_compile_definitions(${NOS_PLUGIN_TARGET} PRIVATE NOS_DISABLE_DEPRECATED)
target_include_directories(${NOS_PLUGIN_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../modules/Plugins/nosUtilities/External/stb)
