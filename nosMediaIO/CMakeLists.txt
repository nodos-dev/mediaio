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

# tinyexr v3 (pure-C11 EXR loader, in the External/tinyexr submodule) with its vendored
# libdeflate as the zlib backend: ~3.5x faster ZIP decode than the legacy tinyexr.h + miniz.
# The C sources sit outside Source/ so the plugin glob won't pick them up - add them directly.
set(TINYEXR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/External/tinyexr")
file(GLOB TINYEXR3_SOURCES "${TINYEXR_DIR}/src/*.c")
set(LIBDEFLATE_SOURCES
    "${TINYEXR_DIR}/deps/libdeflate/lib/adler32.c"
    "${TINYEXR_DIR}/deps/libdeflate/lib/deflate_compress.c"
    "${TINYEXR_DIR}/deps/libdeflate/lib/deflate_decompress.c"
    "${TINYEXR_DIR}/deps/libdeflate/lib/zlib_compress.c"
    "${TINYEXR_DIR}/deps/libdeflate/lib/zlib_decompress.c"
    "${TINYEXR_DIR}/deps/libdeflate/lib/utils.c"
    "${TINYEXR_DIR}/deps/libdeflate/lib/x86/cpu_features.c"
    "${TINYEXR_DIR}/deps/libdeflate/lib/arm/cpu_features.c")
set(TINYEXR_ZSTD_SOURCE "${TINYEXR_DIR}/deps/zstd/tinyexr_zstd.c")
nos_add_plugin("nosMediaIO" "${DEPENDENCIES}" "${TINYEXR_DIR}/include")
target_sources(nosMediaIO PRIVATE ${TINYEXR3_SOURCES} ${LIBDEFLATE_SOURCES} ${TINYEXR_ZSTD_SOURCE})
target_include_directories(nosMediaIO PRIVATE
    "${TINYEXR_DIR}/src" "${TINYEXR_DIR}/deps/zstd" "${TINYEXR_DIR}/deps/libdeflate")
target_compile_definitions(nosMediaIO PRIVATE EXR_USE_LIBDEFLATE EXR_ZLIB_DEFAULT_LIBDEFLATE=1)
# The v3 sources use C11 _Atomic; MSVC needs the experimental switch for it. Third-party
# code, so warnings are silenced.
if(MSVC)
    set_source_files_properties(${TINYEXR3_SOURCES} ${LIBDEFLATE_SOURCES} ${TINYEXR_ZSTD_SOURCE}
        PROPERTIES COMPILE_OPTIONS "/std:c11;/experimental:c11atomics;/w")
else()
    set_source_files_properties(${TINYEXR3_SOURCES} ${LIBDEFLATE_SOURCES} ${TINYEXR_ZSTD_SOURCE}
        PROPERTIES COMPILE_OPTIONS "-std=c11;-w")
endif()

# Project generation
nos_group_targets("nosMediaIO" "NOS Plugins")