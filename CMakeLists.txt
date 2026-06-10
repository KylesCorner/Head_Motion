cmake_minimum_required(VERSION 3.22)

project(headmotion_client
    VERSION 0.1.0
    LANGUAGES C CXX
)

# ============================================================
# Project options
# ============================================================

option(HEADMOTION_BUILD_TESTS "Build unit tests" OFF)

set(HEADMOTION_SERIAL_BACKEND "native" CACHE STRING
    "Serial backend: native or libserialport"
)

set_property(CACHE HEADMOTION_SERIAL_BACKEND PROPERTY STRINGS
    native
    libserialport
)

set(METAWEAR_SDK_DIR
    "/home/kyle/Collage/HeadMotion/code/MetaWear-SDK-Cpp"
    CACHE PATH
    "Path to MetaWear-SDK-Cpp"
)

# Your code is C++20.
# The MetaWear SDK may force its own older C++ settings internally.
# We explicitly set C++20 per target below for our own targets.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# MetaWear SDK
#
# This imports the local MetaWear-SDK-Cpp project.
#
# The SDK headers are located at:
#   ${METAWEAR_SDK_DIR}/src/metawear/...
#
# So our bridge target must include:
#   ${METAWEAR_SDK_DIR}/src
#
# The SDK CMake target appears to be:
#   metawear
# based on your build output:
#   metawear-sdk/CMakeFiles/metawear.dir/...
# ============================================================

add_subdirectory(
    "${METAWEAR_SDK_DIR}"
    "${CMAKE_BINARY_DIR}/metawear-sdk"
)

if(TARGET metawear)
    set(HEADMOTION_METAWEAR_SDK_TARGET metawear)
elseif(TARGET MetaWear)
    set(HEADMOTION_METAWEAR_SDK_TARGET MetaWear)
elseif(TARGET MetaWearSDK)
    set(HEADMOTION_METAWEAR_SDK_TARGET MetaWearSDK)
else()
    message(FATAL_ERROR
        "Could not find MetaWear SDK CMake target. "
        "Expected one of: metawear, MetaWear, MetaWearSDK."
    )
endif()

message(STATUS "Using MetaWear SDK target: ${HEADMOTION_METAWEAR_SDK_TARGET}")
# ============================================================
# Session / state persistence
# ============================================================

add_library(headmotion_session
    src/session/BoardStateStore.cpp
)

target_include_directories(headmotion_session PUBLIC
    include
)

target_link_libraries(headmotion_session PUBLIC
    headmotion_core
)

target_compile_features(headmotion_session PUBLIC cxx_std_20)

# ============================================================
# Core portable utilities
#
# This target contains platform-independent helpers:
#   - Hex parsing / formatting
#   - String helpers later
#   - Logging helpers later
#   - Error / Result types later
#
# This target must not include Linux/macOS/Windows-specific code.
# ============================================================

add_library(headmotion_core
    src/util/Hex.cpp
)

target_include_directories(headmotion_core PUBLIC
    include
)

target_compile_features(headmotion_core PUBLIC cxx_std_20)

# ============================================================
# Portable transport interfaces
#
# This is INTERFACE-only right now.
#
# It owns headers like:
#   include/headmotion/transport/IByteTransport.hpp
#   include/headmotion/transport/SerialConfig.hpp
#   include/headmotion/transport/SerialPortInfo.hpp
#   include/headmotion/transport/SerialPortFactory.hpp
#
# Important:
#   SerialPortFactory.cpp is not compiled here.
#   The factory chooses a platform backend, so it is compiled into
#   headmotion_platform_serial instead.
# ============================================================

add_library(headmotion_transport INTERFACE)

target_include_directories(headmotion_transport INTERFACE
    include
)

target_link_libraries(headmotion_transport INTERFACE
    headmotion_core
)

target_compile_features(headmotion_transport INTERFACE cxx_std_20)

# ============================================================
# Portable USB / MetaWear protocol helpers
#
# This layer knows how to encode/decode the MMS USB frame format:
#
#   1F <payload_len> <payload bytes...> 0A
#
# This target should not know about:
#   - serial ports
#   - Linux/macOS/Windows
#   - BLE
#   - the MetaWear SDK
# ============================================================

add_library(headmotion_protocol
    src/protocol/UsbFrameCodec.cpp
)

target_include_directories(headmotion_protocol PUBLIC
    include
)

target_link_libraries(headmotion_protocol PUBLIC
    headmotion_core
)

target_compile_features(headmotion_protocol PUBLIC cxx_std_20)

# ============================================================
# Portable MetaWear-over-USB transport
#
# This wraps a generic byte transport and applies MMS USB framing.
#
# It depends on:
#   - headmotion_transport for IByteTransport
#   - headmotion_protocol for UsbFrameCodec
#   - headmotion_core for utilities
#
# This target should remain portable.
# It must not include termios, Win32, libserialport, or OS headers.
# ============================================================

add_library(headmotion_metawear_usb
    src/metawear/MetaWearUsbTransport.cpp
)

target_include_directories(headmotion_metawear_usb PUBLIC
    include
)

target_link_libraries(headmotion_metawear_usb PUBLIC
    headmotion_core
    headmotion_transport
    headmotion_protocol
)

target_compile_features(headmotion_metawear_usb PUBLIC cxx_std_20)

# ============================================================
# MetaWear SDK bridge
#
# This target connects the MetaWear SDK callback model to your
# MetaWearUsbTransport.
#
# It depends on:
#   - headmotion_metawear_usb
#   - the imported MetaWear SDK target
#
# Important:
#   MetaWearSdkBridge.hpp includes SDK headers:
#
#       metawear/core/metawearboard.h
#       metawear/platform/btle_connection.h
#
#   Therefore ${METAWEAR_SDK_DIR}/src must be a PUBLIC include dir.
# ============================================================

add_library(headmotion_sdk_bridge
    src/sdk/MetaWearSdkBridge.cpp
)

target_include_directories(headmotion_sdk_bridge PUBLIC
    include
    "${METAWEAR_SDK_DIR}/src"
)

target_link_libraries(headmotion_sdk_bridge PUBLIC
    headmotion_core
    headmotion_metawear_usb
    ${HEADMOTION_METAWEAR_SDK_TARGET}
)

target_compile_features(headmotion_sdk_bridge PUBLIC cxx_std_20)

# ============================================================
# Platform serial backend
#
# This target owns actual serial-port implementations.
#
# For Linux native backend, this compiles:
#   - SerialPortFactory.cpp
#   - LinuxSerialPort.cpp
#   - LinuxSerialDiscovery.cpp
#
# Why SerialPortFactory.cpp lives here:
#   The factory chooses a platform implementation, so it is not
#   purely portable.
#
# Later, this same target can select:
#   - native Windows backend
#   - native macOS backend
#   - libserialport backend
# ============================================================

add_library(headmotion_platform_serial)

target_include_directories(headmotion_platform_serial
    PUBLIC
        include
    PRIVATE
        src
)

target_link_libraries(headmotion_platform_serial PUBLIC
    headmotion_transport
    headmotion_core
)

target_compile_features(headmotion_platform_serial PUBLIC cxx_std_20)

if(HEADMOTION_SERIAL_BACKEND STREQUAL "native")

    if(WIN32)
        message(FATAL_ERROR "Native Windows serial backend is not implemented yet")

    elseif(APPLE)
        message(FATAL_ERROR "Native macOS serial backend is not implemented yet")

    elseif(UNIX)
        target_sources(headmotion_platform_serial PRIVATE
            src/transport/SerialPortFactory.cpp
            src/platform/linux/LinuxSerialPort.cpp
            src/platform/linux/LinuxSerialDiscovery.cpp
        )

        target_compile_definitions(headmotion_platform_serial PUBLIC
            HEADMOTION_PLATFORM_LINUX=1
        )

    else()
        message(FATAL_ERROR "Unsupported native platform")
    endif()

elseif(HEADMOTION_SERIAL_BACKEND STREQUAL "libserialport")

    message(FATAL_ERROR "libserialport backend is not wired up yet")

else()
    message(FATAL_ERROR "Unknown HEADMOTION_SERIAL_BACKEND=${HEADMOTION_SERIAL_BACKEND}")
endif()

# ============================================================
# Application command layer
#
# This target owns CLI command implementations:
#   - scan
#   - identify
#   - tx-raw
#   - cmd
#   - module-info
#   - sdk-probe
#
# This layer may compose portable and platform services.
# It should not directly use low-level OS APIs.
# ============================================================

add_library(headmotion_app
    src/app/ScanPortsCommand.cpp
    src/app/IdentifyCommand.cpp
    src/app/RawTxCommand.cpp
    src/app/CommandPayloadCommand.cpp
    src/app/ModuleInfoCommand.cpp
    src/app/SdkProbeCommand.cpp
    src/app/RecordStartCommand.cpp
    src/app/RecordStopCommand.cpp
    src/app/RecordResetCommand.cpp
    src/app/SyncCommand.cpp
)

target_include_directories(headmotion_app PUBLIC
    include
)

target_link_libraries(headmotion_app PUBLIC
    headmotion_core
    headmotion_session
    headmotion_transport
    headmotion_platform_serial
    headmotion_protocol
    headmotion_metawear_usb
    headmotion_sdk_bridge
)

target_compile_features(headmotion_app PUBLIC cxx_std_20)

# ============================================================
# Executable
#
# Keep main.cpp thin.
#
# main.cpp should:
#   - parse the command name
#   - dispatch to app commands
#   - catch top-level exceptions
#
# It should not contain serial/protocol/SDK logic.
# ============================================================

add_executable(mmsctl
    src/main.cpp
)

target_link_libraries(mmsctl PRIVATE
    headmotion_app
)

target_compile_definitions(mmsctl PRIVATE
    HEADMOTION_VERSION="${PROJECT_VERSION}"
)

target_compile_features(mmsctl PRIVATE cxx_std_20)

# ============================================================
# Warnings
# ============================================================

if(MSVC)
    target_compile_options(headmotion_core PRIVATE /W4)
    target_compile_options(headmotion_protocol PRIVATE /W4)
    target_compile_options(headmotion_metawear_usb PRIVATE /W4)
    target_compile_options(headmotion_sdk_bridge PRIVATE /W4)
    target_compile_options(headmotion_platform_serial PRIVATE /W4)
    target_compile_options(headmotion_app PRIVATE /W4)
    target_compile_options(mmsctl PRIVATE /W4)
else()
    target_compile_options(headmotion_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(headmotion_protocol PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(headmotion_metawear_usb PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(headmotion_sdk_bridge PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(headmotion_platform_serial PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(headmotion_app PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mmsctl PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(headmotion_session PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ============================================================
# Tests
#
# Not wired yet.
#
# Later this can become:
#
# if(HEADMOTION_BUILD_TESTS)
#     enable_testing()
#     add_subdirectory(tests)
# endif()
# ============================================================