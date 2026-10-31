set(_noting_core_sources
    src/common/bytes.cpp
    src/common/unicode.cpp
    src/common/unit.cpp
    src/container/cfb.cpp
    src/container/zip.cpp
    src/model/document.cpp
    src/hwp/records.cpp
    src/hwp/hwp_reader.cpp
    src/hwp/hwp_writer.cpp
    src/hwpx/hwpx_reader.cpp
    src/hwpx/hwpx_writer.cpp
    src/reader.cpp
    src/writer.cpp
    src/export/json.cpp
    src/export/text.cpp
    src/export/layout.cpp
    src/render/scene.cpp
    src/render/line_breaker.cpp
    src/render/scene_edit.cpp
    src/render/line_cache.cpp
    src/render/glyph_path.cpp
    src/render/paint.cpp
    src/render/pointer_hold.cpp
    src/render/painter_draw.cpp
    src/render/picture.cpp
    src/render/border_mesh.cpp
    src/render/border_mesh_check.cpp
    src/render/line_place_check.cpp
    src/render/painter_shape.cpp
    src/render/painter_table.cpp
    src/render/painter_text.cpp
    src/render/painter_caret.cpp
    src/render/edit_commands.cpp
    src/render/format_fields.cpp
    src/render/view.cpp
    src/input/viewport.cpp
    src/input/pointer.cpp
    src/render/frame_cache.cpp
    src/render/cache_budget.cpp
    src/render/lz4_blob.cpp
    third_party/lz4/lz4.c
    src/render/equation_latex.cpp
    src/render/equation_layout.cpp
    src/render/equation_raster.cpp
    src/render/microtex_graphics.cpp
    src/export/font_metrics.cpp
    src/agent/agent.cpp
)

add_library(noting_core STATIC ${_noting_core_sources})
add_library(Noting::core ALIAS noting_core)

target_include_directories(noting_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4)

target_compile_features(noting_core PUBLIC cxx_std_20)

target_link_libraries(noting_core
    PUBLIC  Noting::compiler_options
    PRIVATE Noting::miniz Noting::pugixml Noting::microtex Noting::woff2 freetype)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(noting_core PRIVATE Threads::Threads)
endif()
if(NOTING_HAS_SKIA)
    target_include_directories(noting_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/build/skia")
    if(EMSCRIPTEN OR NOT MSVC)
        target_compile_definitions(noting_core PRIVATE NOTING_HAS_SKIA=1)
        target_link_libraries(noting_core PRIVATE Noting::skia)
    else()
        # 헤더의 SK_DEBUG 는 NDEBUG 가 없을 때 켜진다. 윈도우 Debug 와 디버그 Skia 만 맞는다.
        # 리눅스, 안드로이드, iOS, macOS, WASM 은 위에서 구성 전체를 연결한다.
        target_compile_definitions(noting_core PRIVATE $<$<CONFIG:Debug>:NOTING_HAS_SKIA=1>)
        target_link_libraries(noting_core PRIVATE $<$<CONFIG:Debug>:Noting::skia>)
        if(NOTING_SKIA_DAWN)
            target_compile_definitions(noting_core PRIVATE $<$<CONFIG:Debug>:NOTING_HAS_DAWN=1>)
        endif()
    endif()
endif()
if(EMSCRIPTEN)
    target_compile_definitions(noting_core PRIVATE "NOTING_MICROTEX_RES=\"/microtex-res\"")
    target_compile_options(noting_core PRIVATE -fexceptions -pthread)
    if(NOTING_HAS_SKIA)
        # Graphite Dawn 헤더. 브라우저 WebGPU 심볼은 noting_wasm 링크의 emdawnwebgpu 포트가 채운다.
        target_include_directories(noting_core SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/build/skia/out/dawn-wasm/gen/src/emdawnwebgpu/include"
            "${CMAKE_SOURCE_DIR}/build/skia/third_party/externals/dawn/include")
    endif()

    if(NOTING_HAS_SKIA_SINGLE)
        add_library(noting_core_single STATIC ${_noting_core_sources})
        add_library(Noting::core_single ALIAS noting_core_single)
        target_include_directories(noting_core_single
            PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4)
        target_compile_features(noting_core_single PUBLIC cxx_std_20)
        target_link_libraries(noting_core_single
            PUBLIC  Noting::compiler_options
            PRIVATE Noting::miniz_single Noting::pugixml_single Noting::microtex_single noting_freetype_single Noting::woff2_single Noting::skia_single)
        target_include_directories(noting_core_single SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/build/skia"
            "${CMAKE_SOURCE_DIR}/build/skia/out/dawn-wasm/gen/src/emdawnwebgpu/include"
            "${CMAKE_SOURCE_DIR}/build/skia/third_party/externals/dawn/include")
        target_compile_definitions(noting_core_single PRIVATE
            NOTING_HAS_SKIA=1
            "NOTING_MICROTEX_RES=\"/microtex-res\"")
        target_compile_options(noting_core_single PRIVATE -fexceptions)
    endif()
else()
    target_compile_definitions(noting_core PRIVATE "NOTING_MICROTEX_RES=\"${NOTING_MICROTEX_RES_DIR}\"")
endif()
