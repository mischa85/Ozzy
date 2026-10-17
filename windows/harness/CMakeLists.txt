# ozzy-harness: an Ozzy engine over WinUSB (dev tool, not shipped). From a VS developer prompt:
#   cmake -S . -B build && cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(ozzy_harness C CXX)

set(OZZY ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(ozzy-harness
    main.cpp
    WinUsbBus.cpp
    ${OZZY}/engine/core/OzzyEngine.cpp
    ${OZZY}/engine/devices/ploytec/PloytecEngine.cpp
    ${OZZY}/engine/devices/usbmidi/UsbMidiEngine.cpp
    ${OZZY}/common/devices/ploytec/ploytec_handshake.c
    ${OZZY}/common/devices/ploytec/ploytec_codec.c
    ${OZZY}/common/usb_midi.c
)

# The engine side of the shared memory, in user mode (engine/shared/OzzyPlatform.h)
target_compile_definitions(ozzy-harness PRIVATE OZZY_USER_ENGINE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
set_target_properties(ozzy-harness PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON C_STANDARD 11)

# Commit we build from, logged at startup (common/ozzy_version.h)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${OZZY} describe --always --dirty
                    OUTPUT_VARIABLE OZZY_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(OZZY_GIT_VERSION)
    target_compile_definitions(ozzy-harness PRIVATE OZZY_GIT_VERSION="${OZZY_GIT_VERSION}")
endif()

if(MSVC)
    target_compile_options(ozzy-harness PRIVATE /W3)
endif()
target_link_libraries(ozzy-harness PRIVATE winusb setupapi avrt ole32)
