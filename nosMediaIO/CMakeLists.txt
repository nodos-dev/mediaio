# Copyright MediaZ Teknoloji A.S. All Rights Reserved.
cmake_minimum_required(VERSION 3.24.2)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)

if (NOT WITH_NODOS_WORKSPACE)
    message(FATAL_ERROR "This repo currently does not support builds without Nodos workspace. "
    "Place this repo under nodos-workspace/Module folder and run cmake -S ./Toolchain/CMake -B Build from workspace root.")
endif()
add_compile_definitions(NOS_DISABLE_DEPRECATED)

nos_find_sdk("1.3.0" NOS_PLUGIN_SDK_TARGET NOS_SUBSYSTEM_SDK_TARGET NOS_SDK_DIR)
cmake_path(SET FLATC_EXECUTABLE "${NOS_SDK_DIR}/bin/flatc")

# Dependencies
nos_get_module("nos.sys.vulkan" "6.0" NOS_SYS_VULKAN_TARGET)
nos_generate_flatbuffers("${CMAKE_CURRENT_SOURCE_DIR}/Config" "${CMAKE_CURRENT_SOURCE_DIR}/Source" "cpp" "" nosMediaIO_generated)
set(DEPENDENCIES nosMediaIO_generated ${NOS_SYS_VULKAN_TARGET} ${NOS_PLUGIN_SDK_TARGET})

# tinyexr (with its bundled miniz) lives in the External/tinyexr submodule. Put the
# header dir and miniz dir on the include path (tinyexr does `#include <miniz.h>`); miniz.c
# sits outside Source/ so the plugin glob won't pick it up - add it to the target directly.
set(TINYEXR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/tinyexr")
nos_add_plugin("nosMediaIO" "${DEPENDENCIES}" "${TINYEXR_DIR};${TINYEXR_DIR}/deps/miniz")
target_sources(nosMediaIO PRIVATE "${TINYEXR_DIR}/deps/miniz/miniz.c")

# Project generation
nos_group_targets("nosMediaIO" "NOS Plugins")