add_executable(chwp-agent main.cpp)

if(WIN32)
    # argv 와 ANSI API 를 UTF-8 로 동작시켜 한글 파일 경로를 그대로 받는다.
    target_sources(chwp-agent PRIVATE chwp-agent.manifest)
endif()

target_link_libraries(chwp-agent PRIVATE chwp::core)
