# Skia 정적 라이브러리. build/skia 를 GN으로 만든 뒤에만 연결한다.
# 그리는 코드는 플랫폼이 같고, 산출물 폴더만 다르다.
#   out/noting-win/skia.lib       Windows Debug (SK_DEBUG, /MDd)
#   out/noting-wasm-pthread/libskia.a  Emscripten (-pthread. 칸 스레드와 한 wasm에 묶인다)
#   out/noting-wasm/libskia.a          Emscripten, 스레드 없는 이전 산출물
#   out/noting-linux/libskia.a    Linux
#   out/noting-android/libskia.a  Android
#   out/noting-ios/libskia.a      iOS
#   out/noting-mac/libskia.a      macOS
set(_noting_skia_root "${CMAKE_SOURCE_DIR}/build/skia")
if(EMSCRIPTEN)
    set(_noting_skia_dir "noting-wasm-pthread")
    set(_noting_skia_name "libskia.a")
elseif(ANDROID)
    set(_noting_skia_dir "noting-android")
    set(_noting_skia_name "libskia.a")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_noting_skia_dir "noting-ios")
    set(_noting_skia_name "libskia.a")
elseif(APPLE)
    set(_noting_skia_dir "noting-mac")
    set(_noting_skia_name "libskia.a")
elseif(WIN32)
    set(_noting_skia_dir "noting-win")
    set(_noting_skia_name "skia.lib")
    # 켜면 Graphite Dawn(D3D12·Vulkan)이 든 out/noting-win-dawn 을 연결한다. 따로 빌드 폴더를 쓴다.
    option(NOTING_SKIA_DAWN "Link the native Graphite Dawn Skia (out/noting-win-dawn)" OFF)
    if(NOTING_SKIA_DAWN)
        set(_noting_skia_dir "noting-win-dawn")
    endif()
else()
    set(_noting_skia_dir "noting-linux")
    set(_noting_skia_name "libskia.a")
endif()
set(_noting_skia_lib "${_noting_skia_root}/out/${_noting_skia_dir}/${_noting_skia_name}")

set(NOTING_HAS_SKIA OFF)
if(EXISTS "${_noting_skia_lib}")
    set(NOTING_HAS_SKIA ON)
    add_library(Noting::skia STATIC IMPORTED)
    set_target_properties(Noting::skia PROPERTIES
        IMPORTED_LOCATION "${_noting_skia_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_noting_skia_root}"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_noting_skia_root}")
    message(STATUS "Skia: ${_noting_skia_lib}")
    if(NOTING_SKIA_DAWN)
        set(_noting_dawn_out "${_noting_skia_root}/out/${_noting_skia_dir}")
        set_property(TARGET Noting::skia APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
            "${_noting_dawn_out}/gen/third_party/dawn/include"
            "${_noting_skia_root}/third_party/externals/dawn/include")
        set_property(TARGET Noting::skia APPEND PROPERTY INTERFACE_LINK_LIBRARIES
            "${_noting_dawn_out}/dawn_combined.lib" onecore.lib user32.lib delayimp.lib dxguid.lib)
        set_property(TARGET Noting::skia APPEND PROPERTY INTERFACE_LINK_OPTIONS "/WHOLEARCHIVE:dawn_combined.lib")
        message(STATUS "Skia Dawn: ${_noting_dawn_out}/dawn_combined.lib")
    endif()
else()
    message(STATUS "Skia 없음. 이 경로에 정적 라이브러리를 두면 연결한다: ${_noting_skia_lib}")
endif()

set(NOTING_HAS_SKIA_SINGLE OFF)
if(EMSCRIPTEN)
    set(_noting_skia_single_lib "${_noting_skia_root}/out/noting-wasm/libskia.a")
    if(EXISTS "${_noting_skia_single_lib}")
        set(NOTING_HAS_SKIA_SINGLE ON)
        add_library(Noting::skia_single STATIC IMPORTED)
        set_target_properties(Noting::skia_single PROPERTIES
            IMPORTED_LOCATION "${_noting_skia_single_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_noting_skia_root}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_noting_skia_root}")
        message(STATUS "Skia (single-thread): ${_noting_skia_single_lib}")
    endif()
endif()
