# ozzyctl for Windows (macOS and Linux: the Makefile). From a VS developer prompt:
#   cmake -S . -B build && cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(ozzyctl C)

add_executable(ozzyctl ozzyctl.c ozzyctl_core.c backend_windows.c)
target_compile_definitions(ozzyctl PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
set_target_properties(ozzyctl PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(ozzyctl PRIVATE /W3)
endif()
target_link_libraries(ozzyctl PRIVATE setupapi cfgmgr32)
