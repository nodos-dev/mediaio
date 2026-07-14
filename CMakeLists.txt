# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

target_compile_definitions(${NOS_PLUGIN_TARGET} PRIVATE NOS_DISABLE_DEPRECATED)
target_include_directories(${NOS_PLUGIN_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/External/stb)

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
target_sources(${NOS_PLUGIN_TARGET} PRIVATE ${TINYEXR3_SOURCES} ${LIBDEFLATE_SOURCES} ${TINYEXR_ZSTD_SOURCE})
target_include_directories(${NOS_PLUGIN_TARGET} PRIVATE
    "${TINYEXR_DIR}/include" "${TINYEXR_DIR}/src" "${TINYEXR_DIR}/deps/zstd" "${TINYEXR_DIR}/deps/libdeflate")
target_compile_definitions(${NOS_PLUGIN_TARGET} PRIVATE EXR_USE_LIBDEFLATE EXR_ZLIB_DEFAULT_LIBDEFLATE=1)
# The v3 sources use C11 _Atomic; MSVC needs the experimental switch for it. Third-party
# code, so warnings are silenced.
#
# This is a sibling CMakeLists.txt, add_subdirectory'd into a target created
# in a different directory scope (the auto-generated plugin target), so a
# plain set_source_files_properties() call here wouldn't be visible where the
# target actually compiles these sources. TARGET_DIRECTORY (CMake >=3.18)
# sets the property as if called from the target's own directory. Kept as
# per-source COMPILE_OPTIONS rather than target_compile_options with a
# COMPILE_LANGUAGE generator expression: the VS generator silently drops
# flags it doesn't recognize (like /experimental:c11atomics) when they're
# gated by COMPILE_LANGUAGE, instead of preserving them in AdditionalOptions.
if(MSVC)
    set_source_files_properties(${TINYEXR3_SOURCES} ${LIBDEFLATE_SOURCES} ${TINYEXR_ZSTD_SOURCE}
        TARGET_DIRECTORY ${NOS_PLUGIN_TARGET}
        PROPERTIES COMPILE_OPTIONS "/std:c11;/experimental:c11atomics;/w")
else()
    set_source_files_properties(${TINYEXR3_SOURCES} ${LIBDEFLATE_SOURCES} ${TINYEXR_ZSTD_SOURCE}
        TARGET_DIRECTORY ${NOS_PLUGIN_TARGET}
        PROPERTIES COMPILE_OPTIONS "-std=c11;-w")
endif()
