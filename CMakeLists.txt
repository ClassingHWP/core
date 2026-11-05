cmake_minimum_required(VERSION 3.24)

project(ClassingHWP
    VERSION 0.2.0
    DESCRIPTION "ClassingHWP - Korean word processor"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(EMSCRIPTEN)
    set(_chwp_native OFF)
else()
    set(_chwp_native ON)
endif()

option(CHWP_BUILD_TOOLS "Build native command line tools" ${_chwp_native})
option(CHWP_BUILD_WASM "Build WebAssembly bindings (Emscripten only)" ${EMSCRIPTEN})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ChwpCompilerOptions)
include(ChwpDependencies)
include(ChwpSkia)

add_subdirectory(core)

if(CHWP_BUILD_TOOLS)
    add_subdirectory(tools/chwp-dump)
    add_subdirectory(tools/chwp-agent)
endif()

if(CHWP_BUILD_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "CHWP_BUILD_WASM requires the Emscripten toolchain")
    endif()
    add_subdirectory(bindings/wasm)
endif()
