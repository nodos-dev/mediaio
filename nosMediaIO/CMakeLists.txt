# Copyright MediaZ Teknoloji A.S. All Rights Reserved.
cmake_minimum_required(VERSION 3.24.2)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)

if (NOT WITH_NODOS_WORKSPACE)
    message(FATAL_ERROR "This repo currently does not support builds without Nodos workspace. "
    "Place this repo under nodos-workspace/Module folder and run cmake -S ./Toolchain/CMake -B Build from workspace root.")
endif()
add_compile_definitions(NOS_DISABLE_DEPRECATED)

nos_find_plugin_sdk("39.12.0" NOS_PLUGIN_SDK_TARGET NOS_SDK_DIR)

# Dependencies
nos_get_plugin("nos.sys.vulkan" "6.28" NOS_SYS_VULKAN_TARGET)
nos_generate_flatbuffers("${CMAKE_CURRENT_SOURCE_DIR}/Config" "${CMAKE_CURRENT_SOURCE_DIR}/Source" "cpp" "" nosMediaIO_generated)
set(DEPENDENCIES nosMediaIO_generated ${NOS_SYS_VULKAN_TARGET} ${NOS_PLUGIN_SDK_TARGET})

nos_add_plugin("nosMediaIO" "${DEPENDENCIES}" "")

# Project generation
nos_group_targets("nosMediaIO" "NOS Plugins")