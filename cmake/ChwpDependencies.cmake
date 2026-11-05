# 외부 의존성: 모두 순수 C/C++ 이며 Emscripten 에서도 빌드된다.
#   - miniz    : ZIP(HWPX) 읽기, raw deflate(HWP 스트림) 해제
#   - pugixml  : HWPX XML 파싱
#   - freetype : 본문 글자 윤곽. 플랫폼 글꼴 API 를 쓰지 않는다.
#
# 오프라인 빌드 시 FETCHCONTENT_SOURCE_DIR_MINIZ / FETCHCONTENT_SOURCE_DIR_PUGIXML /
# FETCHCONTENT_SOURCE_DIR_FREETYPE 로 미리 받아 둔 소스 경로를 지정할 수 있다.

include(FetchContent)

# SOURCE_SUBDIR 를 존재하지 않는 경로로 지정해 upstream CMakeLists 를 쓰지 않고
# 소스만 받은 뒤, 필요한 옵션으로 직접 타깃을 만든다.
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ADA38DB0B703A56D3DD6D57BF84A9C5D664921D870D8FEA4DB153979FB5332C5
    SOURCE_SUBDIR _chwp_no_cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
    URL_HASH SHA256=2F10E276870C64B1DB6809050A75E11A897A8D7456C4BE5C6B2E35A11168A015
    SOURCE_SUBDIR _chwp_no_cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
    URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
    URL_HASH SHA256=bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(miniz pugixml freetype)

add_library(chwp_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
add_library(chwp::miniz ALIAS chwp_miniz)
target_include_directories(chwp_miniz SYSTEM PUBLIC "${miniz_SOURCE_DIR}")
target_compile_definitions(chwp_miniz PUBLIC
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(chwp_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(chwp_pugixml STATIC "${pugixml_SOURCE_DIR}/src/pugixml.cpp")
add_library(chwp::pugixml ALIAS chwp_pugixml)
target_include_directories(chwp_pugixml SYSTEM PUBLIC "${pugixml_SOURCE_DIR}/src")
target_compile_definitions(chwp_pugixml PUBLIC
    PUGIXML_NO_XPATH
    PUGIXML_NO_EXCEPTIONS)
target_compile_features(chwp_pugixml PUBLIC cxx_std_11)
set_target_properties(chwp_pugixml PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(EMSCRIPTEN)
    target_compile_options(chwp_miniz PRIVATE -pthread)
    target_compile_options(chwp_pugixml PRIVATE -pthread)
    target_compile_options(freetype PRIVATE -pthread)

    add_library(chwp_miniz_single STATIC "${miniz_SOURCE_DIR}/miniz.c")
    add_library(chwp::miniz_single ALIAS chwp_miniz_single)
    target_include_directories(chwp_miniz_single SYSTEM PUBLIC "${miniz_SOURCE_DIR}")
    target_compile_definitions(chwp_miniz_single PUBLIC
        MINIZ_NO_STDIO
        MINIZ_NO_TIME
        MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    set_target_properties(chwp_miniz_single PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_library(chwp_pugixml_single STATIC "${pugixml_SOURCE_DIR}/src/pugixml.cpp")
    add_library(chwp::pugixml_single ALIAS chwp_pugixml_single)
    target_include_directories(chwp_pugixml_single SYSTEM PUBLIC "${pugixml_SOURCE_DIR}/src")
    target_compile_definitions(chwp_pugixml_single PUBLIC
        PUGIXML_NO_XPATH
        PUGIXML_NO_EXCEPTIONS)
    target_compile_features(chwp_pugixml_single PUBLIC cxx_std_11)
    set_target_properties(chwp_pugixml_single PROPERTIES POSITION_INDEPENDENT_CODE ON)

    get_target_property(_chwp_ft_raw_sources freetype SOURCES)
    set(_chwp_ft_single_sources)
    foreach(_ft_src IN LISTS _chwp_ft_raw_sources)
        if(_ft_src MATCHES "\\.(c|cc|cpp)$")
            if(IS_ABSOLUTE "${_ft_src}")
                list(APPEND _chwp_ft_single_sources "${_ft_src}")
            else()
                list(APPEND _chwp_ft_single_sources "${freetype_SOURCE_DIR}/${_ft_src}")
            endif()
        endif()
    endforeach()
    add_library(chwp_freetype_single STATIC ${_chwp_ft_single_sources})
    target_include_directories(chwp_freetype_single SYSTEM PUBLIC
        "${freetype_BINARY_DIR}/include"
        "${freetype_SOURCE_DIR}/include")
    target_compile_definitions(chwp_freetype_single PRIVATE FT2_BUILD_LIBRARY)
    set_target_properties(chwp_freetype_single PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(chwp_freetype_single PRIVATE -w)
endif()

include(ChwpMicrotex)

# WOFF2 는 셸이 넘긴 바이트를 코어 메모리의 sfnt 로 푼다. 주소는 코어가 모른다.
FetchContent_Declare(brotli
    URL https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz
    URL_HASH SHA256=e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff
    SOURCE_SUBDIR _chwp_no_cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(woff2
    URL https://github.com/google/woff2/archive/refs/tags/v1.0.2.tar.gz
    URL_HASH SHA256=add272bb09e6384a4833ffca4896350fdb16e0ca22df68c0384773c67a175594
    SOURCE_SUBDIR _chwp_no_cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(brotli woff2)

set(_chwp_brotli_sources
    c/common/constants.c
    c/common/context.c
    c/common/dictionary.c
    c/common/platform.c
    c/common/shared_dictionary.c
    c/common/transform.c
    c/dec/bit_reader.c
    c/dec/decode.c
    c/dec/huffman.c
    c/dec/state.c)
set(_chwp_woff2_sources
    src/table_tags.cc
    src/variable_length.cc
    src/woff2_common.cc
    src/woff2_dec.cc
    src/woff2_out.cc)

function(chwp_add_woff2 target)
    set(sources)
    foreach(src IN LISTS _chwp_brotli_sources)
        list(APPEND sources "${brotli_SOURCE_DIR}/${src}")
    endforeach()
    foreach(src IN LISTS _chwp_woff2_sources)
        list(APPEND sources "${woff2_SOURCE_DIR}/${src}")
    endforeach()
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} SYSTEM PUBLIC
        "${brotli_SOURCE_DIR}/c/include"
        "${woff2_SOURCE_DIR}/include")
    target_include_directories(${target} PRIVATE "${woff2_SOURCE_DIR}/src")
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /w)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
    if(EMSCRIPTEN AND NOT target STREQUAL "chwp_woff2_single")
        target_compile_options(${target} PRIVATE -pthread)
    endif()
endfunction()

chwp_add_woff2(chwp_woff2)
add_library(chwp::woff2 ALIAS chwp_woff2)
if(EMSCRIPTEN)
    chwp_add_woff2(chwp_woff2_single)
    add_library(chwp::woff2_single ALIAS chwp_woff2_single)
endif()
