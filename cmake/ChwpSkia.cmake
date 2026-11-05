# Skia 정적 라이브러리. 모든 빌드가 Skia 로 그린다. build/skia 를 GN 으로 먼저 만들어야 한다.
# 그리는 코드는 플랫폼이 같고, 산출물 폴더만 다르다. 네이티브는 Graphite Dawn(GPU)이 든 Skia 를 기본으로 쓴다.
# GPU 장치가 없으면 core 가 CPU 래스터로 그리므로, 쓸 GPU 가 없는 곳(claude.ai 샌드박스 등)도 같은 Skia 를 연결한다.
#   out/chwp-win-dawn/skia.lib          Windows Debug, Dawn D3D12 (SK_DEBUG, /MDd)
#   out/chwp-win-dawn-release/skia.lib  Windows Release, Dawn D3D12 (/MD)
#   out/chwp-win/skia.lib, out/chwp-win-release/skia.lib  Windows, GPU 없음(CHWP_SKIA_DAWN=OFF)
#   out/chwp-wasm-pthread/libskia.a  Emscripten (-pthread. 칸 스레드와 한 wasm에 묶인다. WebGPU 는 브라우저가 준다)
#   out/chwp-wasm/libskia.a          Emscripten, 스레드 없는 이전 산출물
#   out/chwp-linux/libskia.a    Linux (Dawn 이면 libdawn_combined.a 를 같은 폴더에)
#   out/chwp-linux-musl/libskia.a  Linux 정적(alpine, chwp-agent 패키지), Dawn Vulkan. CHWP_SKIA_OUT=chwp-linux-musl 로 고른다
#   out/chwp-android/libskia.a  Android
#   out/chwp-ios/libskia.a      iOS
#   out/chwp-mac/libskia.a      macOS
set(_chwp_skia_root "${CMAKE_SOURCE_DIR}/build/skia")
if(NOT EMSCRIPTEN)
    option(CHWP_SKIA_DAWN "Link the native Graphite Dawn Skia" ON)
endif()
set(_chwp_skia_name "libskia.a")
set(_chwp_dawn_name "libdawn_combined.a")
if(EMSCRIPTEN)
    set(_chwp_skia_dir "chwp-wasm-pthread")
elseif(ANDROID)
    set(_chwp_skia_dir "chwp-android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_chwp_skia_dir "chwp-ios")
elseif(APPLE)
    set(_chwp_skia_dir "chwp-mac")
elseif(WIN32)
    set(_chwp_skia_name "skia.lib")
    set(_chwp_dawn_name "dawn_combined.lib")
    if(CHWP_SKIA_DAWN)
        set(_chwp_skia_dir "chwp-win-dawn")
        set(_chwp_skia_release_dir "chwp-win-dawn-release")
    else()
        set(_chwp_skia_dir "chwp-win")
        set(_chwp_skia_release_dir "chwp-win-release")
    endif()
else()
    set(_chwp_skia_dir "chwp-linux")
endif()
set(CHWP_SKIA_OUT "" CACHE STRING "build/skia/out 아래 Skia 산출물 폴더 이름(비우면 플랫폼 기본)")
if(CHWP_SKIA_OUT)
    set(_chwp_skia_dir "${CHWP_SKIA_OUT}")
    unset(_chwp_skia_release_dir)
endif()
set(_chwp_skia_out "${_chwp_skia_root}/out/${_chwp_skia_dir}")
set(_chwp_skia_lib "${_chwp_skia_out}/${_chwp_skia_name}")

if(NOT EXISTS "${_chwp_skia_lib}")
    message(FATAL_ERROR "Skia 가 없습니다. build/skia 에서 GN 으로 먼저 빌드하세요: ${_chwp_skia_lib}")
endif()
set(CHWP_HAS_SKIA ON)
add_library(chwp::skia STATIC IMPORTED)
set_target_properties(chwp::skia PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_chwp_skia_root}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_chwp_skia_root}")
if(MSVC AND _chwp_skia_release_dir)
    # 헤더의 SK_DEBUG 는 NDEBUG 가 없을 때 켜진다. Debug 는 디버그 Skia(/MDd), 나머지는 Release Skia(/MD)를 쓴다.
    set(_chwp_skia_release_out "${_chwp_skia_root}/out/${_chwp_skia_release_dir}")
    set(_chwp_skia_release_lib "${_chwp_skia_release_out}/${_chwp_skia_name}")
    if(NOT EXISTS "${_chwp_skia_release_lib}")
        message(FATAL_ERROR "Release Skia 가 없습니다: ${_chwp_skia_release_lib}")
    endif()
    set_target_properties(chwp::skia PROPERTIES
        IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
        IMPORTED_LOCATION_DEBUG "${_chwp_skia_lib}"
        IMPORTED_LOCATION_RELEASE "${_chwp_skia_release_lib}"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
        MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE)
    set(_chwp_dawn_lib "$<IF:$<CONFIG:Debug>,${_chwp_skia_out},${_chwp_skia_release_out}>/${_chwp_dawn_name}")
    message(STATUS "Skia: ${_chwp_skia_lib} (Debug), ${_chwp_skia_release_lib}")
else()
    set_target_properties(chwp::skia PROPERTIES IMPORTED_LOCATION "${_chwp_skia_lib}")
    set(_chwp_dawn_lib "${_chwp_skia_out}/${_chwp_dawn_name}")
    message(STATUS "Skia: ${_chwp_skia_lib}")
endif()
if(CHWP_SKIA_DAWN)
    # Graphite Dawn. 헤더는 Dawn 이 만든 webgpu 헤더(gen)와 Dawn 원본 include 다.
    set_property(TARGET chwp::skia APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
        "${_chwp_skia_out}/gen/third_party/dawn/include"
        "${_chwp_skia_root}/third_party/externals/dawn/include")
    if(MSVC)
        set_property(TARGET chwp::skia APPEND PROPERTY INTERFACE_LINK_LIBRARIES
            "${_chwp_dawn_lib}" onecore.lib user32.lib delayimp.lib dxguid.lib)
        set_property(TARGET chwp::skia APPEND PROPERTY INTERFACE_LINK_OPTIONS "/WHOLEARCHIVE:${_chwp_dawn_name}")
    else()
        set_property(TARGET chwp::skia APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${_chwp_dawn_lib}" ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "Skia Dawn: ${_chwp_dawn_lib}")
endif()

set(CHWP_HAS_SKIA_SINGLE OFF)
if(EMSCRIPTEN)
    set(_chwp_skia_single_lib "${_chwp_skia_root}/out/chwp-wasm/libskia.a")
    if(EXISTS "${_chwp_skia_single_lib}")
        set(CHWP_HAS_SKIA_SINGLE ON)
        add_library(chwp::skia_single STATIC IMPORTED)
        set_target_properties(chwp::skia_single PROPERTIES
            IMPORTED_LOCATION "${_chwp_skia_single_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_chwp_skia_root}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_chwp_skia_root}")
        message(STATUS "Skia (single-thread): ${_chwp_skia_single_lib}")
    endif()
endif()
