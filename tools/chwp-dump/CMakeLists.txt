add_executable(chwp-dump main.cpp)

if(WIN32)
    # argv 와 ANSI API 를 UTF-8 로 동작시켜 한글 파일 경로를 그대로 받는다.
    target_sources(chwp-dump PRIVATE chwp-dump.manifest)
endif()

target_link_libraries(chwp-dump PRIVATE chwp::core)
target_include_directories(chwp-dump PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
