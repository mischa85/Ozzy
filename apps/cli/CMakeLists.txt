# ozzy-cli for Windows (macOS and Linux: the Makefile). From a VS developer prompt:
#   cmake -S . -B build && cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(ozzy-cli C)

add_executable(ozzy-cli ozzy_cli.c ../control/ozzy_control_core.c ../control/backend_windows.c)
target_compile_definitions(ozzy-cli PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
set_target_properties(ozzy-cli PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(ozzy-cli PRIVATE /W3)
endif()
target_link_libraries(ozzy-cli PRIVATE setupapi cfgmgr32)
