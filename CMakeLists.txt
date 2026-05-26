cmake_minimum_required(VERSION 3.20)
project(HeadMotion LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(METAWEAR_SDK_DIR "$ENV{HOME}/Collage/HeadMotion/code/MetaWear-SDK-Cpp")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

add_library(headmotion_core INTERFACE)

target_include_directories(headmotion_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(headmotion_storage
    src/storage/FileMetadataStore.cpp
)

target_link_libraries(headmotion_storage PUBLIC
    headmotion_core
)

add_library(headmotion_ble
    src/ble/SimpleBleAdapter.cpp
    src/ble/SimpleBleInfoBoard.cpp
    src/ble/MetaWearBoardAdapter.cpp
)

target_include_directories(headmotion_ble PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${METAWEAR_SDK_DIR}/src
)

target_link_directories(headmotion_ble PUBLIC
    ${METAWEAR_SDK_DIR}/dist/release/lib/x64
)

target_link_libraries(headmotion_ble PUBLIC
    headmotion_core
    simpleble
    metawear
    PkgConfig::DBUS
)

target_link_options(headmotion_ble PUBLIC
    "-Wl,-rpath,${METAWEAR_SDK_DIR}/dist/release/lib/x64"
)

add_executable(headmotion
    src/main.cpp
)

target_link_libraries(headmotion PRIVATE
    headmotion_core
    headmotion_ble
    headmotion_storage
)

add_executable(test_record_start_service
    test/test_record_start_service.cpp
)

target_include_directories(test_record_start_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test/support
)

target_link_libraries(test_record_start_service PRIVATE
    headmotion_core
)

add_executable(test_scan_service
    test/test_scan_service.cpp
)

target_include_directories(test_scan_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test/support
)

target_link_libraries(test_scan_service PRIVATE
    headmotion_core
)

add_executable(test_info_service
    test/test_info_service.cpp
)

target_include_directories(test_info_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test/support
)

target_link_libraries(test_info_service PRIVATE
    headmotion_core
)

add_executable(test_sync_service
    test/test_sync_service.cpp
)

target_include_directories(test_sync_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/test/support
)

target_link_libraries(test_sync_service PRIVATE
    headmotion_core
)

enable_testing()

add_test(
    NAME test_record_start_service
    COMMAND test_record_start_service
)

add_test(
    NAME test_scan_service
    COMMAND test_scan_service
)

add_test(
    NAME test_info_service
    COMMAND test_info_service
)

add_test(
    NAME test_sync_service
    COMMAND test_sync_service
)
