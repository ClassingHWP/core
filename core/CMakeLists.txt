set(_chwp_core_sources
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
    src/render/paragraph_layout.cpp
    src/render/glyph_path.cpp
    src/render/paint.cpp
    src/render/edit_state.cpp
    src/render/pointer_hold.cpp
    src/render/painter_draw.cpp
    src/render/picture.cpp
    src/render/picture_upload.cpp
    src/render/border_mesh.cpp
    src/render/border_mesh_check.cpp
    src/render/line_place_check.cpp
    src/render/font_probe.cpp
    src/render/painter_shape.cpp
    src/render/painter_table.cpp
    src/render/painter_text.cpp
    src/render/painter_caret.cpp
    src/render/edit_commands.cpp
    src/render/undo.cpp
    src/render/format_fields.cpp
    src/render/format_view.cpp
    src/render/view.cpp
    src/input/viewport.cpp
    src/input/pointer.cpp
    src/input/wheel_reader.cpp
    src/input/hangul.cpp
    src/input/text_field.cpp
    src/render/frame_cache.cpp
    src/render/cache_budget.cpp
    src/render/screen_ops.cpp
    src/render/frame_phase.cpp
    src/render/screen_op_canvas.cpp
    src/render/page_texture_cache.cpp
    src/render/lz4_blob.cpp
    third_party/lz4/lz4.c
    src/render/equation_draw.cpp
    src/render/equation_layout.cpp
    src/export/font_metrics.cpp
    src/agent/agent.cpp
    src/agent/agent_call.cpp
    src/app/doc_session.cpp
    src/app/session.cpp
    src/app/frame_stats.cpp
)

add_library(chwp_core STATIC ${_chwp_core_sources})
add_library(chwp::core ALIAS chwp_core)

target_include_directories(chwp_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4)

target_compile_features(chwp_core PUBLIC cxx_std_20)

target_link_libraries(chwp_core
    PUBLIC  chwp::compiler_options
    PRIVATE chwp::miniz chwp::pugixml chwp::woff2 freetype)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(chwp_core PRIVATE Threads::Threads)
endif()
# 모든 빌드가 Skia 로 그린다(ChwpSkia 가 없으면 구성에서 멈춘다). 윈도우는 구성마다 맞는 Skia 를 고른다.
target_include_directories(chwp_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/build/skia")
target_compile_definitions(chwp_core PRIVATE CHWP_HAS_SKIA=1)
target_link_libraries(chwp_core PRIVATE chwp::skia)
if(CHWP_SKIA_DAWN)
    # 내부 헤더(painter.hpp 등)의 GPU 구조가 시험과 같도록 PUBLIC 으로 둔다.
    target_compile_definitions(chwp_core PUBLIC CHWP_HAS_DAWN=1)
endif()
if(NOT MSVC)
    # Skia 는 RTTI 없이 빌드된다. Skia 캔버스를 이어받는 파일은 typeinfo 를 찾지 않게 RTTI 를 끈다.
    set_source_files_properties(src/render/screen_op_canvas.cpp PROPERTIES COMPILE_OPTIONS -fno-rtti)
endif()
if(EMSCRIPTEN)
    target_compile_options(chwp_core PRIVATE -fexceptions -pthread)
    if(CHWP_HAS_SKIA)
        # Graphite Dawn 헤더. 브라우저 WebGPU 심볼은 chwp_wasm 링크의 emdawnwebgpu 포트가 채운다.
        target_include_directories(chwp_core SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/build/skia/out/dawn-wasm/gen/src/emdawnwebgpu/include"
            "${CMAKE_SOURCE_DIR}/build/skia/third_party/externals/dawn/include")
    endif()

    if(CHWP_HAS_SKIA_SINGLE)
        add_library(chwp_core_single STATIC ${_chwp_core_sources})
        add_library(chwp::core_single ALIAS chwp_core_single)
        target_include_directories(chwp_core_single
            PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
                    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4)
        target_compile_features(chwp_core_single PUBLIC cxx_std_20)
        target_link_libraries(chwp_core_single
            PUBLIC  chwp::compiler_options
            PRIVATE chwp::miniz_single chwp::pugixml_single chwp_freetype_single chwp::woff2_single chwp::skia_single)
        target_include_directories(chwp_core_single SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/build/skia"
            "${CMAKE_SOURCE_DIR}/build/skia/out/dawn-wasm/gen/src/emdawnwebgpu/include"
            "${CMAKE_SOURCE_DIR}/build/skia/third_party/externals/dawn/include")
        target_compile_definitions(chwp_core_single PRIVATE CHWP_HAS_SKIA=1)
        target_compile_options(chwp_core_single PRIVATE -fexceptions)
    endif()
endif()
