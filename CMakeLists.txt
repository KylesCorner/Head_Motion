cmake_minimum_required(VERSION 3.22)

project(headmotion_client
    VERSION 1.1.0
    LANGUAGES C CXX
)

# ============================================================
# Project options
# ============================================================

option(HEADMOTION_BUILD_TESTS "Build unit tests" OFF)
option(HEADMOTION_BUILD_GUI "Build FLTK GUI" ON)

set(
    HEADMOTION_SERIAL_BACKEND
    "native"
    CACHE STRING
    "Serial backend: native or libserialport"
)

set_property(
    CACHE HEADMOTION_SERIAL_BACKEND
    PROPERTY STRINGS
        native
        libserialport
)

set(
    METAWEAR_SDK_DIR
    "${CMAKE_SOURCE_DIR}/external/MetaWear-SDK-Cpp"
    CACHE PATH
    "Path to MetaWear-SDK-Cpp"
)

if(NOT EXISTS "${METAWEAR_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "MetaWear SDK not found at:\n"
        "  ${METAWEAR_SDK_DIR}\n\n"
        "Expected repository layout:\n"
        "  external/MetaWear-SDK-Cpp\n\n"
        "Clone it with:\n"
        "  git clone https://github.com/mbientlab/MetaWear-SDK-Cpp.git "
        "external/MetaWear-SDK-Cpp\n"
        "  git -C external/MetaWear-SDK-Cpp submodule update --init --recursive"
    )
endif()

# Our code uses C++20. The MetaWear SDK configures its own standard.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# Core portable utilities
# ============================================================

add_library(headmotion_core
    src/util/Hex.cpp
)

target_include_directories(headmotion_core PUBLIC
    include
)

target_compile_features(headmotion_core PUBLIC
    cxx_std_20
)

# ============================================================
# Session / state persistence
# ============================================================

add_library(headmotion_session
    src/session/BoardStateStore.cpp
    src/session/DevicePortStore.cpp
)

target_include_directories(headmotion_session PUBLIC
    include
)

target_link_libraries(headmotion_session PUBLIC
    headmotion_core
)

target_compile_features(headmotion_session PUBLIC
    cxx_std_20
)

# ============================================================
# Portable transport interfaces
# ============================================================

add_library(headmotion_transport INTERFACE)

target_include_directories(headmotion_transport INTERFACE
    include
)

target_link_libraries(headmotion_transport INTERFACE
    headmotion_core
)

target_compile_features(headmotion_transport INTERFACE
    cxx_std_20
)

# ============================================================
# Portable USB / MetaWear protocol helpers
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

target_compile_features(headmotion_protocol PUBLIC
    cxx_std_20
)

# ============================================================
# Portable MetaWear-over-USB transport
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

target_compile_features(headmotion_metawear_usb PUBLIC
    cxx_std_20
)

# ============================================================
# MetaWear SDK
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

message(
    STATUS
    "Using MetaWear SDK target: ${HEADMOTION_METAWEAR_SDK_TARGET}"
)

# ============================================================
# MetaWear SDK bridge
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

target_compile_features(headmotion_sdk_bridge PUBLIC
    cxx_std_20
)

# ============================================================
# Platform serial backend
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

target_compile_features(headmotion_platform_serial PUBLIC
    cxx_std_20
)

if(HEADMOTION_SERIAL_BACKEND STREQUAL "native")

    if(WIN32)

        target_sources(headmotion_platform_serial PRIVATE
            src/transport/SerialPortFactory.cpp
            src/platform/windows/WindowsSerialPort.cpp
            src/platform/windows/WindowsSerialDiscovery.cpp
        )

        target_compile_definitions(
            headmotion_platform_serial
            PUBLIC
                HEADMOTION_PLATFORM_WINDOWS=1
            PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )

        target_link_libraries(
            headmotion_platform_serial
            PRIVATE
                setupapi
                advapi32
        )

    elseif(APPLE)

        message(FATAL_ERROR
            "Native macOS serial backend is not implemented yet"
        )

    elseif(UNIX)

        target_sources(headmotion_platform_serial PRIVATE
            src/transport/SerialPortFactory.cpp
            src/platform/linux/LinuxSerialPort.cpp
            src/platform/linux/LinuxSerialDiscovery.cpp
        )

        target_compile_definitions(
            headmotion_platform_serial
            PUBLIC
                HEADMOTION_PLATFORM_LINUX=1
        )

    else()

        message(FATAL_ERROR
            "Unsupported native platform"
        )

    endif()

elseif(HEADMOTION_SERIAL_BACKEND STREQUAL "libserialport")

    message(FATAL_ERROR
        "libserialport backend is not wired up yet"
    )

else()

    message(FATAL_ERROR
        "Unknown HEADMOTION_SERIAL_BACKEND=${HEADMOTION_SERIAL_BACKEND}"
    )

endif()

# ============================================================
# Application command layer
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
    src/app/MmsDeviceProbe.cpp
    src/app/DevicePortResolver.cpp
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

target_compile_features(headmotion_app PUBLIC
    cxx_std_20
)

# ============================================================
# CLI executable
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

target_compile_features(mmsctl PRIVATE
    cxx_std_20
)

# ============================================================
# FLTK GUI
# ============================================================

if(HEADMOTION_BUILD_GUI)

    find_package(FLTK CONFIG REQUIRED)

    # FLTK 1.4 commonly exports fltk::fltk.
    # Some FLTK 1.3 packages export fltk.
    if(TARGET fltk::fltk)
        set(HEADMOTION_FLTK_TARGET fltk::fltk)
    elseif(TARGET fltk)
        set(HEADMOTION_FLTK_TARGET fltk)
    else()
        message(FATAL_ERROR
            "FLTK was found, but no supported FLTK target exists. "
            "Expected fltk::fltk or fltk."
        )
    endif()

    add_executable(headmotion_gui
        src/gui/main.cpp
        src/gui/MainWindow.cpp
    )

    target_include_directories(headmotion_gui PRIVATE
        include
    )

    if(FLTK_INCLUDE_DIRS)
        target_include_directories(headmotion_gui PRIVATE
            ${FLTK_INCLUDE_DIRS}
        )
    endif()

    target_link_libraries(headmotion_gui PRIVATE
        headmotion_app
        ${HEADMOTION_FLTK_TARGET}
    )

    target_compile_features(headmotion_gui PRIVATE
        cxx_std_20
    )

    if(WIN32)
        set_target_properties(headmotion_gui PROPERTIES
            WIN32_EXECUTABLE TRUE
        )
    endif()

endif()

# ============================================================
# Warnings
# ============================================================

if(MSVC)

    target_compile_options(headmotion_core PRIVATE /W4)
    target_compile_options(headmotion_session PRIVATE /W4)
    target_compile_options(headmotion_protocol PRIVATE /W4)
    target_compile_options(headmotion_metawear_usb PRIVATE /W4)
    target_compile_options(headmotion_sdk_bridge PRIVATE /W4)
    target_compile_options(headmotion_platform_serial PRIVATE /W4)
    target_compile_options(headmotion_app PRIVATE /W4)
    target_compile_options(mmsctl PRIVATE /W4)

    if(HEADMOTION_BUILD_GUI)
        target_compile_options(headmotion_gui PRIVATE /W4)
    endif()

else()

    target_compile_options(
        headmotion_core
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        headmotion_session
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        headmotion_protocol
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        headmotion_metawear_usb
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        headmotion_sdk_bridge
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        headmotion_platform_serial
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        headmotion_app
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    target_compile_options(
        mmsctl
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    if(HEADMOTION_BUILD_GUI)
        target_compile_options(
            headmotion_gui
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()

endif()

# ============================================================
# Tests
# ============================================================

if(HEADMOTION_BUILD_TESTS)
    enable_testing()

    # Tests can be added here later.
endif()

# ============================================================
# Installation
# ============================================================

include(GNUInstallDirs)

install(
    TARGETS
        mmsctl
    RUNTIME DESTINATION
        ${CMAKE_INSTALL_BINDIR}
)

if(HEADMOTION_BUILD_GUI)
    install(
        TARGETS
            headmotion_gui
        RUNTIME DESTINATION
            ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(UNIX AND NOT APPLE AND HEADMOTION_BUILD_GUI)

    install(
        FILES
            packaging/linux/headmotion.desktop
        DESTINATION
            share/applications
    )

    install(
        FILES
            packaging/linux/headmotion.png
        DESTINATION
            share/icons/hicolor/256x256/apps
    )

endif()

# ============================================================
# Linux AppImage runtime dependencies
# ============================================================

if(UNIX AND NOT APPLE AND HEADMOTION_BUILD_GUI)

    install(CODE [[
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES
                $<TARGET_FILE:headmotion_gui>
                $<TARGET_FILE:mmsctl>

            RESOLVED_DEPENDENCIES_VAR
                HEADMOTION_RUNTIME_DEPS

            UNRESOLVED_DEPENDENCIES_VAR
                HEADMOTION_UNRESOLVED_DEPS

            POST_EXCLUDE_REGEXES
                ".*/libc\\.so\\..*"
                ".*/ld-linux.*\\.so.*"
                ".*/libm\\.so\\..*"
                ".*/libpthread\\.so\\..*"
                ".*/libdl\\.so\\..*"
                ".*/librt\\.so\\..*"
                ".*/libresolv\\.so\\..*"
        )

        file(MAKE_DIRECTORY
            "${CMAKE_INSTALL_PREFIX}/lib"
        )

        foreach(DEP ${HEADMOTION_RUNTIME_DEPS})

            file(COPY
                "${DEP}"
                DESTINATION
                "${CMAKE_INSTALL_PREFIX}/lib"
            )

            file(REAL_PATH
                "${DEP}"
                RESOLVED_DEP
            )

            file(COPY
                "${RESOLVED_DEP}"
                DESTINATION
                "${CMAKE_INSTALL_PREFIX}/lib"
            )

        endforeach()

        if(HEADMOTION_UNRESOLVED_DEPS)
            message(WARNING
                "Unresolved AppImage dependencies: "
                "${HEADMOTION_UNRESOLVED_DEPS}"
            )
        endif()
    ]])

endif()

# ============================================================
# Packaging
# ============================================================

set(CPACK_PACKAGE_NAME "HeadMotion")
set(CPACK_PACKAGE_VENDOR "HeadMotion")

set(
    CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "MMS+ recording and data download utility"
)

set(
    CPACK_PACKAGE_VERSION
    "${PROJECT_VERSION}"
)

set(
    CPACK_PACKAGE_INSTALL_DIRECTORY
    "HeadMotion"
)

if(WIN32)

    set(CPACK_GENERATOR "NSIS")

    set(
        CPACK_NSIS_DISPLAY_NAME
        "HeadMotion"
    )

    set(
        CPACK_NSIS_PACKAGE_NAME
        "HeadMotion"
    )

    set(
        CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL
        ON
    )

elseif(APPLE)

    set(CPACK_GENERATOR "DragNDrop")

elseif(UNIX)

    set(CPACK_GENERATOR "AppImage")

    set(
        CPACK_PACKAGE_ICON
        "headmotion.png"
    )

    set(
        CPACK_APPIMAGE_DESKTOP_FILE
        "headmotion.desktop"
    )

endif()

include(CPack)
