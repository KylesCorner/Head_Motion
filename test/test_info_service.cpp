#include "headmotion/app/InfoService.h"

#include "FakeMetaWearBoard.h"
#include "TestLogger.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace headmotion;
using namespace headmotion::test;

#define CHECK_TRUE(expr)                                                        \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__              \
                      << " expected true: " #expr "\n";                       \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define CHECK_FALSE(expr)                                                       \
    do {                                                                        \
        if ((expr)) {                                                           \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__              \
                      << " expected false: " #expr "\n";                      \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        const auto& av = (a);                                                    \
        const auto& bv = (b);                                                    \
        if (!(av == bv)) {                                                       \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__              \
                      << " expected " #a " == " #b "\n";                     \
            std::cerr << "       actual:   " << av << "\n";                   \
            std::cerr << "       expected: " << bv << "\n";                   \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

static void test_info_connects_and_initializes_board() {
    FakeMetaWearBoard board;
    TestLogger logger;

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.connectCalled);
    CHECK_TRUE(board.initializeCalled);
    CHECK_EQ(board.connectedTarget, std::string("MetaWear"));
}

static void test_info_reads_device_information() {
    FakeMetaWearBoard board;
    TestLogger logger;

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.readDeviceInfoCalled);

    CHECK_EQ(metadata.deviceMac, std::string("F9:CB:94:04:C3:45"));
    CHECK_EQ(metadata.manufacturer, std::string("MbientLab Inc"));
    CHECK_EQ(metadata.modelNumber, std::string("8"));
    CHECK_EQ(metadata.serialNumber, std::string("0561E1"));
    CHECK_EQ(metadata.firmwareRevision, std::string("1.7.2"));
    CHECK_EQ(metadata.hardwareRevision, std::string("0.1"));
}

static void test_info_disconnects_on_success() {
    FakeMetaWearBoard board;
    TestLogger logger;

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.disconnectCalled);
}

static void test_info_does_not_initialize_if_connect_fails() {
    FakeMetaWearBoard board;
    TestLogger logger;

    board.connectResult = Result::error("BLE connection failed");

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(board.connectCalled);
    CHECK_FALSE(board.initializeCalled);
    CHECK_FALSE(board.readDeviceInfoCalled);
    CHECK_FALSE(board.disconnectCalled);
    CHECK_EQ(logger.errorMessages.size(), static_cast<size_t>(1));
}

static void test_info_disconnects_if_initialize_fails() {
    FakeMetaWearBoard board;
    TestLogger logger;

    board.initializeResult = Result::error("MetaWear init failed");

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(board.connectCalled);
    CHECK_TRUE(board.initializeCalled);
    CHECK_FALSE(board.readDeviceInfoCalled);
    CHECK_TRUE(board.disconnectCalled);
    CHECK_EQ(logger.errorMessages.size(), static_cast<size_t>(1));
}

static void test_info_fails_if_board_reports_not_initialized() {
    FakeMetaWearBoard board;
    TestLogger logger;

    board.initialized = false;

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(board.connectCalled);
    CHECK_TRUE(board.initializeCalled);
    CHECK_FALSE(board.readDeviceInfoCalled);
    CHECK_TRUE(board.disconnectCalled);
}

static void test_info_disconnects_if_read_device_info_fails() {
    FakeMetaWearBoard board;
    TestLogger logger;

    board.readDeviceInfoResult = Result::error("DIS read failed");

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(board.connectCalled);
    CHECK_TRUE(board.initializeCalled);
    CHECK_TRUE(board.readDeviceInfoCalled);
    CHECK_TRUE(board.disconnectCalled);
    CHECK_EQ(logger.errorMessages.size(), static_cast<size_t>(1));
}

static void test_info_does_not_call_recording_methods() {
    FakeMetaWearBoard board;
    TestLogger logger;

    InfoService service(board, logger);

    SessionMetadata metadata;
    Result result = service.run("MetaWear", metadata);

    CHECK_TRUE(result.ok());

    CHECK_FALSE(board.clearLogEntriesCalled);
    CHECK_FALSE(board.configureSignalsCalled);
    CHECK_FALSE(board.createLoggersCalled);
    CHECK_FALSE(board.startLoggingCalled);
    CHECK_FALSE(board.stopLoggingCalled);
    CHECK_FALSE(board.flushLogPageCalled);
    CHECK_FALSE(board.subscribeLoggersCalled);
    CHECK_FALSE(board.downloadLogsCalled);
}

int main() {
    test_info_connects_and_initializes_board();
    test_info_reads_device_information();
    test_info_disconnects_on_success();
    test_info_does_not_initialize_if_connect_fails();
    test_info_disconnects_if_initialize_fails();
    test_info_fails_if_board_reports_not_initialized();
    test_info_disconnects_if_read_device_info_fails();
    test_info_does_not_call_recording_methods();

    std::cout << "[PASS] InfoService tests\n";
    return 0;
}
