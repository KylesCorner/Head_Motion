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

option(
    HEADMOTION_STATIC_MINGW_RUNTIME
    "Statically link the MinGW runtime into Windows executables"
    ON
)

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

if(CMAKE_CROSSCOMPILING AND WIN32)
    message(STATUS "Cross-compiling HeadMotion for Windows")
endif()

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
# MinGW runtime
#
# The Arch Linux -> Windows cross build uses the static vcpkg
# triplet.  Link the MinGW runtime statically as well so the
# resulting EXEs do not require libgcc_s_seh-1.dll,
# libstdc++-6.dll, or libwinpthread-1.dll beside them.
# ============================================================

if(MINGW AND HEADMOTION_STATIC_MINGW_RUNTIME)

    target_link_options(mmsctl PRIVATE
        -static
    )

    if(HEADMOTION_BUILD_GUI)
        target_link_options(headmotion_gui PRIVATE
            -static
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

    find_package(
        Threads REQUIRED
    )


    # ========================================================
    # Common test helper
    # ========================================================
    #
    # Usage:
    #
    #   headmotion_add_test_target(
    #       <target>
    #       <source>
    #       <ctest-name>
    #
    #       [EXTRA_SOURCES ...]
    #       [LIBRARIES ...]
    #       [LABELS ...]
    #       [TIMEOUT <seconds>]
    #       [SKIP_RETURN_77]
    #       [RUN_SERIAL]
    #   )
    #
    # Every HeadMotion test:
    #   - includes project headers
    #   - uses C++20
    #   - is registered with CTest
    #
    # Individual tests only specify what differs.
    # ========================================================

    function(
        headmotion_add_test_target
        TARGET_NAME
        SOURCE_FILE
        TEST_NAME
    )

        set(
            options
                SKIP_RETURN_77
                RUN_SERIAL
        )

        set(
            one_value_args
                TIMEOUT
        )

        set(
            multi_value_args
                EXTRA_SOURCES
                LIBRARIES
                LABELS
        )

        cmake_parse_arguments(
            TEST
            "${options}"
            "${one_value_args}"
            "${multi_value_args}"
            ${ARGN}
        )


        add_executable(
            ${TARGET_NAME}
            ${SOURCE_FILE}
            ${TEST_EXTRA_SOURCES}
        )


        target_include_directories(
            ${TARGET_NAME}
            PRIVATE
                include
        )


        target_compile_features(
            ${TARGET_NAME}
            PRIVATE
                cxx_std_20
        )


        if(TEST_LIBRARIES)

            target_link_libraries(
                ${TARGET_NAME}
                PRIVATE
                    ${TEST_LIBRARIES}
            )

        endif()


        add_test(
            NAME
                ${TEST_NAME}
            COMMAND
                ${TARGET_NAME}
        )


        if(TEST_LABELS)

            set_property(
                TEST
                    ${TEST_NAME}
                PROPERTY
                    LABELS
                    ${TEST_LABELS}
            )

        endif()


        if(TEST_TIMEOUT)

            set_property(
                TEST
                    ${TEST_NAME}
                PROPERTY
                    TIMEOUT
                    ${TEST_TIMEOUT}
            )

        endif()


        if(TEST_SKIP_RETURN_77)

            set_property(
                TEST
                    ${TEST_NAME}
                PROPERTY
                    SKIP_RETURN_CODE
                    77
            )

        endif()


        if(TEST_RUN_SERIAL)

            set_property(
                TEST
                    ${TEST_NAME}
                PROPERTY
                    RUN_SERIAL
                    TRUE
            )

        endif()

    endfunction()


    # ========================================================
    # White-box SyncCommand tests
    # ========================================================
    #
    # These tests include SyncCommand.cpp directly so they can
    # exercise its anonymous-namespace/private data-path helpers.
    #
    # MmsDeviceProbe.cpp must therefore also be compiled into
    # each test to satisfy symbols referenced by SyncCommand.cpp.
    # ========================================================

    set(
        HEADMOTION_SYNC_TEST_LIBRARIES

        headmotion_core
        headmotion_session
        headmotion_transport
        headmotion_platform_serial
        headmotion_protocol
        headmotion_metawear_usb
        headmotion_sdk_bridge
        Threads::Threads
    )


    function(
        headmotion_add_sync_test
        TARGET_NAME
        SOURCE_FILE
        TEST_NAME
    )

        headmotion_add_test_target(
            ${TARGET_NAME}
            ${SOURCE_FILE}
            ${TEST_NAME}

            EXTRA_SOURCES
                src/app/MmsDeviceProbe.cpp

            LIBRARIES
                ${HEADMOTION_SYNC_TEST_LIBRARIES}

            ${ARGN}
        )

    endfunction()


    # --------------------------------------------------------
    # Large synthetic sync
    # --------------------------------------------------------

    headmotion_add_sync_test(
        headmotion_test_sync_large
        tests/test_sync_large.cpp
        sync_large

        LABELS
            sync
            integration
            stress

        TIMEOUT
            120
    )


    # --------------------------------------------------------
    # Parallel synthetic sync
    # --------------------------------------------------------

    headmotion_add_sync_test(
        headmotion_test_parallel_sync
        tests/test_parallel_sync.cpp
        parallel_sync

        LABELS
            sync
            parallel
            integration
            stress

        TIMEOUT
            300
    )


    # --------------------------------------------------------
    # Accel / gyro pairing
    # --------------------------------------------------------

    headmotion_add_sync_test(
        headmotion_test_sync_pairing
        tests/test_sync_pairing.cpp
        sync_pairing

        LABELS
            sync
            unit
            pairing

        TIMEOUT
            30
    )


    # --------------------------------------------------------
    # Queue overflow
    # --------------------------------------------------------

    headmotion_add_sync_test(
        headmotion_test_sync_queue_overflow
        tests/test_sync_queue_overflow.cpp
        sync_queue_overflow

        LABELS
            sync
            unit
            queue
            failure

        TIMEOUT
            30
    )


    # --------------------------------------------------------
    # Parallel failure isolation
    # --------------------------------------------------------

    headmotion_add_sync_test(
        headmotion_test_parallel_failure_isolation
        tests/test_parallel_failure_isolation.cpp
        parallel_failure_isolation

        LABELS
            sync
            parallel
            failure
            integration

        TIMEOUT
            120
    )


    # --------------------------------------------------------
    # Filename collision handling
    # --------------------------------------------------------

    headmotion_add_sync_test(
        headmotion_test_sync_filename_collision
        tests/test_sync_filename_collision.cpp
        sync_filename_collision

        LABELS
            sync
            filesystem
            parallel
            unit

        TIMEOUT
            30
    )


    # --------------------------------------------------------
    # Device ID filename sanitization
    # --------------------------------------------------------

    headmotion_add_sync_test(
        headmotion_test_device_id_sanitization
        tests/test_device_id_sanitization.cpp
        device_id_sanitization

        LABELS
            sync
            unit
            filesystem
            security

        TIMEOUT
            10
    )


    # ========================================================
    # Protocol tests
    # ========================================================

    headmotion_add_test_target(
        headmotion_test_usb_frame_codec
        tests/test_usb_frame_codec.cpp
        usb_frame_codec

        LIBRARIES
            headmotion_protocol

        LABELS
            protocol
            usb
            unit

        TIMEOUT
            10
    )


    # ========================================================
    # CommandOutput tests
    # ========================================================

    headmotion_add_test_target(
        headmotion_test_command_output_concurrency
        tests/test_command_output_concurrency.cpp
        command_output_concurrency

        LIBRARIES
            Threads::Threads

        LABELS
            output
            parallel
            unit
            stress

        TIMEOUT
            120
    )


    # ========================================================
    # Hardware test helper
    # ========================================================
    #
    # Hardware tests:
    #   - link against the real application layer
    #   - return 77 when hardware/configuration is unavailable
    #   - never run alongside another CTest test
    #
    # This prevents separate destructive hardware tests from
    # fighting over the same physical MMS+.
    # ========================================================

    function(
        headmotion_add_hardware_test
        TARGET_NAME
        SOURCE_FILE
        TEST_NAME
        TIMEOUT_SECONDS
    )

        headmotion_add_test_target(
            ${TARGET_NAME}
            ${SOURCE_FILE}
            ${TEST_NAME}

            LIBRARIES
                headmotion_app
                Threads::Threads

            LABELS
                hardware
                ${ARGN}

            TIMEOUT
                ${TIMEOUT_SECONDS}

            SKIP_RETURN_77
            RUN_SERIAL
        )

    endfunction()
    
    # ========================================================
    # Hardware: device scan/discovery
    #
    # Non-destructive, but actively probes serial ports.
    #
    # Opt in with:
    #
    #   HEADMOTION_SCAN_EXPECTED_DEVICES=1
    #
    # Optional exact hardware IDs:
    #
    #   HEADMOTION_SCAN_EXPECTED_IDS="0561E1,ABC123"
    # ========================================================

    headmotion_add_test_target(
        headmotion_test_hardware_scan
        tests/test_hardware_scan.cpp
        hardware_scan

        LIBRARIES
            headmotion_app
            Threads::Threads

        LABELS
            hardware
            scan
            integration

        TIMEOUT
            120

        SKIP_RETURN_77
        RUN_SERIAL
    )


    # ========================================================
    # Hardware: parallel operations
    # ========================================================

    headmotion_add_hardware_test(
        headmotion_test_parallel_operations
        tests/test_parallel_operations.cpp
        parallel_operations_hw
        300

        parallel
        integration
    )


    # ========================================================
    # Hardware: single-device E2E
    # ========================================================

    headmotion_add_hardware_test(
        headmotion_test_hardware_single_device_e2e
        tests/test_hardware_single_device_e2e.cpp
        hardware_single_device_e2e
        180

        integration
        e2e
        destructive
    )


    # ========================================================
    # Hardware: multi-device sequential E2E
    #
    # Devices are tested one at a time.
    #
    # Opt in with:
    #
    #   HEADMOTION_HW_E2E_PORTS="/dev/serial/by-id/a,/dev/serial/by-id/b"
    #
    # This test clears existing recordings.
    # ========================================================

    headmotion_add_hardware_test(
        headmotion_test_hardware_multi_device_e2e
        tests/test_hardware_multi_device_e2e.cpp
        hardware_multi_device_e2e
        600

        multi-device
        integration
        e2e
        destructive
    )
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
