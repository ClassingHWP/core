# MicroTeX (cLaTeXMath, MIT) 를 noting 안에 넣는다.
# upstream CMake 는 Qt/Skia/GDI 를 고르므로 쓰지 않고, 코어 소스만 정적 라이브러리로 만든다.
# 그리기 구현은 core 쪽 stb 백엔드다. WASM 도 같은 라이브러리를 링크한다.

include(FetchContent)

FetchContent_Declare(tinyxml2
    URL https://github.com/leethomason/tinyxml2/archive/refs/tags/10.0.0.tar.gz
    URL_HASH SHA256=3bdf15128ba16686e69bce256cc468e76c7b94ff2c7f391cc5ec09e40bff3839
    SOURCE_SUBDIR _noting_no_cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(microtex
    URL https://github.com/NanoMichael/MicroTeX/archive/0e3707f6dafebb121d98b53c64364d16fefe481d.zip
    URL_HASH SHA256=9da82a0d05ff4edec080938570ac86e3024c8d461c2d739a5eb3b6259aa69c1a
    SOURCE_SUBDIR _noting_no_cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(tinyxml2 microtex)

# HOME 이 있을 때 asprintf 버퍼를 delete 하면 WASM 이 죽는다. free 로만 고친다.
file(READ "${microtex_SOURCE_DIR}/src/latex.cpp" _noting_microtex_latex)
string(REPLACE "delete userdata_fallback;" "free(userdata_fallback);" _noting_microtex_latex_patched "${_noting_microtex_latex}")
if(NOT _noting_microtex_latex STREQUAL _noting_microtex_latex_patched)
    file(WRITE "${microtex_SOURCE_DIR}/src/latex.cpp" "${_noting_microtex_latex_patched}")
endif()

file(READ "${microtex_SOURCE_DIR}/src/utils/exceptions.h" _noting_microtex_ex)
string(REPLACE "const exception&" "const std::exception&" _noting_microtex_ex_patched "${_noting_microtex_ex}")
if(NOT _noting_microtex_ex STREQUAL _noting_microtex_ex_patched)
    file(WRITE "${microtex_SOURCE_DIR}/src/utils/exceptions.h" "${_noting_microtex_ex_patched}")
endif()

file(TO_CMAKE_PATH "${microtex_SOURCE_DIR}/res" NOTING_MICROTEX_RES_DIR)

add_library(noting_tinyxml2 STATIC "${tinyxml2_SOURCE_DIR}/tinyxml2.cpp")
add_library(Noting::tinyxml2 ALIAS noting_tinyxml2)
target_include_directories(noting_tinyxml2 SYSTEM PUBLIC "${tinyxml2_SOURCE_DIR}")
set_target_properties(noting_tinyxml2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(noting_tinyxml2 PRIVATE /utf-8 /W0)
else()
    target_compile_options(noting_tinyxml2 PRIVATE -w)
endif()

set(_noting_microtex_sources
    src/atom/atom_basic.cpp
    src/atom/atom_char.cpp
    src/atom/atom_impl.cpp
    src/atom/atom_matrix.cpp
    src/atom/atom_row.cpp
    src/atom/atom_space.cpp
    src/atom/colors_def.cpp
    src/atom/unit_conversion.cpp
    src/box/box.cpp
    src/box/box_factory.cpp
    src/box/box_group.cpp
    src/box/box_single.cpp
    src/core/core.cpp
    src/core/formula.cpp
    src/core/formula_def.cpp
    src/core/glue.cpp
    src/core/localized_num.cpp
    src/core/macro.cpp
    src/core/macro_def.cpp
    src/core/macro_impl.cpp
    src/core/parser.cpp
    src/fonts/alphabet.cpp
    src/fonts/font_basic.cpp
    src/fonts/font_info.cpp
    src/fonts/fonts.cpp
    src/utils/string_utils.cpp
    src/utils/utf.cpp
    src/utils/utils.cpp
    src/res/builtin/formula_mappings.res.cpp
    src/res/builtin/symbol_mapping.res.cpp
    src/res/builtin/tex_param.res.cpp
    src/res/builtin/tex_symbols.res.cpp
    src/res/font/bi10.def.cpp
    src/res/font/bx10.def.cpp
    src/res/font/cmbsy10.def.cpp
    src/res/font/cmbx10.def.cpp
    src/res/font/cmbxti10.def.cpp
    src/res/font/cmex10.def.cpp
    src/res/font/cmmi10.def.cpp
    src/res/font/cmmi10_unchanged.def.cpp
    src/res/font/cmmib10.def.cpp
    src/res/font/cmmib10_unchanged.def.cpp
    src/res/font/cmr10.def.cpp
    src/res/font/cmss10.def.cpp
    src/res/font/cmssbx10.def.cpp
    src/res/font/cmssi10.def.cpp
    src/res/font/cmsy10.def.cpp
    src/res/font/cmti10.def.cpp
    src/res/font/cmti10_unchanged.def.cpp
    src/res/font/cmtt10.def.cpp
    src/res/font/dsrom10.def.cpp
    src/res/font/eufb10.def.cpp
    src/res/font/eufm10.def.cpp
    src/res/font/i10.def.cpp
    src/res/font/moustache.def.cpp
    src/res/font/msam10.def.cpp
    src/res/font/msbm10.def.cpp
    src/res/font/r10.def.cpp
    src/res/font/r10_unchanged.def.cpp
    src/res/font/rsfs10.def.cpp
    src/res/font/sb10.def.cpp
    src/res/font/sbi10.def.cpp
    src/res/font/si10.def.cpp
    src/res/font/special.def.cpp
    src/res/font/ss10.def.cpp
    src/res/font/stmary10.def.cpp
    src/res/font/tt10.def.cpp
    src/res/parser/font_parser.cpp
    src/res/parser/formula_parser.cpp
    src/res/reg/builtin_font_reg.cpp
    src/res/reg/builtin_syms_reg.cpp
    src/res/sym/amsfonts.def.cpp
    src/res/sym/amssymb.def.cpp
    src/res/sym/base.def.cpp
    src/res/sym/stmaryrd.def.cpp
    src/res/sym/symspecial.def.cpp
    src/latex.cpp
    src/render.cpp
)
list(TRANSFORM _noting_microtex_sources PREPEND "${microtex_SOURCE_DIR}/")

add_library(noting_microtex STATIC ${_noting_microtex_sources})
add_library(Noting::microtex ALIAS noting_microtex)
target_include_directories(noting_microtex SYSTEM PUBLIC "${microtex_SOURCE_DIR}/src")
target_link_libraries(noting_microtex PUBLIC Noting::tinyxml2)
target_compile_features(noting_microtex PUBLIC cxx_std_17)
set_target_properties(noting_microtex PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(noting_microtex PRIVATE /utf-8 /EHsc /W0)
else()
    target_compile_options(noting_microtex PRIVATE -w)
endif()
if(EMSCRIPTEN)
    target_compile_options(noting_tinyxml2 PRIVATE -pthread)
    target_compile_options(noting_microtex PRIVATE -fexceptions -pthread)

    add_library(noting_tinyxml2_single STATIC "${tinyxml2_SOURCE_DIR}/tinyxml2.cpp")
    add_library(Noting::tinyxml2_single ALIAS noting_tinyxml2_single)
    target_include_directories(noting_tinyxml2_single SYSTEM PUBLIC "${tinyxml2_SOURCE_DIR}")
    set_target_properties(noting_tinyxml2_single PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(noting_tinyxml2_single PRIVATE -w)

    add_library(noting_microtex_single STATIC ${_noting_microtex_sources})
    add_library(Noting::microtex_single ALIAS noting_microtex_single)
    target_include_directories(noting_microtex_single SYSTEM PUBLIC "${microtex_SOURCE_DIR}/src")
    target_link_libraries(noting_microtex_single PUBLIC Noting::tinyxml2_single)
    target_compile_features(noting_microtex_single PUBLIC cxx_std_17)
    set_target_properties(noting_microtex_single PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(noting_microtex_single PRIVATE -w -fexceptions)
endif()
