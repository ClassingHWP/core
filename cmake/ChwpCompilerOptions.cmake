add_library(chwp_compiler_options INTERFACE)
add_library(chwp::compiler_options ALIAS chwp_compiler_options)

if(MSVC)
    target_compile_options(chwp_compiler_options INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus)
    target_compile_definitions(chwp_compiler_options INTERFACE
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(chwp_compiler_options INTERFACE
        -Wall -Wextra -Wpedantic)
endif()
