add_executable(chwp_wasm chwp_wasm.cpp)
target_link_libraries(chwp_wasm PRIVATE chwp::core)
target_include_directories(chwp_wasm PRIVATE ${CMAKE_SOURCE_DIR}/core/src)

set_target_properties(chwp_wasm PROPERTIES
    OUTPUT_NAME "chwp"
    SUFFIX ".mjs")

set(_chwp_emdawn "${CMAKE_SOURCE_DIR}/build/skia/out/dawn-wasm/emdawnwebgpu_pkg/emdawnwebgpu.port.py")
# 링크 전에 JS 라이브러리를 고친다. Dawn 패키지를 다시 만들어도 같은 복사가 들어간다.
add_custom_command(TARGET chwp_wasm PRE_LINK
    COMMAND python "${CMAKE_SOURCE_DIR}/bindings/wasm/patch-emdawn-heap.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM)
target_compile_options(chwp_wasm PRIVATE -fexceptions -pthread "SHELL:--use-port=${_chwp_emdawn}")
target_link_options(chwp_wasm PRIVATE
    -lembind
    "SHELL:--use-port=${_chwp_emdawn}"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createChwpModule"
    "-sALLOW_MEMORY_GROWTH=1"
    # 1은 브라우저가 키울 수 있는 버퍼를 지원하면 쓰고, 없으면 예전 방식으로 는다.
    # 2로 올리면 경고는 사라지지만 파이어폭스 154 전에서는 메모리 증가가 깨진다.
    "-sGROWABLE_ARRAYBUFFERS=1"
    "-Wno-pthreads-mem-growth"
    "-sENVIRONMENT=web,worker,node"
    "-pthread"
    "SHELL:-sPTHREAD_POOL_SIZE=Math.max(4,Math.min(8,(navigator.hardwareConcurrency||4)))"
    "-sDEFAULT_PTHREAD_STACK_SIZE=1048576"
    # 문서 조판은 이 스레드에서 돈다. 표·글상자 안 개체를 다시 조판하며 깊이 재귀하므로
    # Emscripten 기본값(64KB)으로는 모자란다. 그리는 스레드와 같은 크기로 둔다.
    "-sSTACK_SIZE=1048576"
    -fexceptions
    "-sFORCE_FILESYSTEM=1"
    "SHELL:--preload-file ${CHWP_MICROTEX_RES_DIR}@/microtex-res")

if(TARGET chwp::core_single)
    add_executable(chwp_wasm_single chwp_wasm.cpp)
    target_link_libraries(chwp_wasm_single PRIVATE chwp::core_single)
    target_include_directories(chwp_wasm_single PRIVATE ${CMAKE_SOURCE_DIR}/core/src)

    set_target_properties(chwp_wasm_single PROPERTIES
        OUTPUT_NAME "chwp-single"
        SUFFIX ".mjs")

    add_custom_command(TARGET chwp_wasm_single PRE_LINK
        COMMAND python "${CMAKE_SOURCE_DIR}/bindings/wasm/patch-emdawn-heap.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM)
    target_compile_options(chwp_wasm_single PRIVATE -fexceptions "SHELL:--use-port=${_chwp_emdawn}")
    target_link_options(chwp_wasm_single PRIVATE
        -lembind
        "SHELL:--use-port=${_chwp_emdawn}"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sEXPORT_NAME=createChwpModule"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sENVIRONMENT=web,worker,node"
        "-sSTACK_SIZE=1048576"
        -fexceptions
        "-sFORCE_FILESYSTEM=1"
        "SHELL:--preload-file ${CHWP_MICROTEX_RES_DIR}@/microtex-res")

    add_dependencies(chwp_wasm chwp_wasm_single)
endif()
