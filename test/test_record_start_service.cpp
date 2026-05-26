#include "headmotion/app/RecordStartService.h"

#include "FakeMetaWearBoard.h"
#include "FakeMetadataStore.h"
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
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

static void test_record_start_connects_and_initializes_board() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;
    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.connectCalled);
    CHECK_TRUE(board.initializeCalled);
    CHECK_EQ(board.connectedTarget, std::string("MetaWear"));
}

static void test_record_start_reads_device_info_and_saves_metadata() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;
    config.accelHz = 50.0f;
    config.gyroHz = 50.0f;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.readDeviceInfoCalled);
    CHECK_TRUE(metadata.saveCalled);
    CHECK_TRUE(metadata.savedMetadata.has_value());

    CHECK_EQ(metadata.savedMetadata->deviceMac, std::string("F9:CB:94:04:C3:45"));
    CHECK_EQ(metadata.savedMetadata->serialNumber, std::string("0561E1"));
    CHECK_EQ(metadata.savedMetadata->firmwareRevision, std::string("1.7.2"));
    CHECK_EQ(metadata.savedMetadata->accelLoggerId, 0);
    CHECK_EQ(metadata.savedMetadata->gyroLoggerId, 1);
}

static void test_record_start_configures_accel_and_gyro() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;
    config.accelHz = 100.0f;
    config.gyroHz = 100.0f;
    config.accelRangeG = 16.0f;
    config.gyroRangeDps = 2000.0f;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.configureSignalsCalled);
    CHECK_EQ(board.lastConfig.accelHz, 100.0f);
    CHECK_EQ(board.lastConfig.gyroHz, 100.0f);
    CHECK_EQ(board.lastConfig.accelRangeG, 16.0f);
    CHECK_EQ(board.lastConfig.gyroRangeDps, 2000.0f);
}

static void test_record_start_erases_existing_logs_when_requested() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;
    config.eraseBeforeStart = true;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.clearLogEntriesCalled);
}

static void test_record_start_does_not_erase_existing_logs_by_default() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;
    config.eraseBeforeStart = false;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_FALSE(board.clearLogEntriesCalled);
}

static void test_record_start_starts_logging_after_metadata_save() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(metadata.saveCalled);
    CHECK_TRUE(board.startLoggingCalled);
    CHECK_TRUE(board.startLoggingOverwriteValue);
}

static void test_record_start_disconnects_on_success() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.disconnectCalled);
}

static void test_record_start_does_not_start_logging_if_connect_fails() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    board.connectResult = Result::error("BLE connection failed");

    RecordStartService service(board, metadata, logger);

    SessionConfig config;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(board.connectCalled);
    CHECK_FALSE(board.initializeCalled);
    CHECK_FALSE(board.startLoggingCalled);
    CHECK_FALSE(metadata.saveCalled);
}

static void test_record_start_does_not_start_logging_if_logger_creation_fails() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    board.createLoggersResult = Result::error("Logger creation failed");

    RecordStartService service(board, metadata, logger);

    SessionConfig config;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(board.createLoggersCalled);
    CHECK_FALSE(board.startLoggingCalled);
    CHECK_FALSE(metadata.saveCalled);
}

static void test_record_start_does_not_start_logging_if_metadata_save_fails() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    metadata.saveResult = Result::error("Could not write metadata file");

    RecordStartService service(board, metadata, logger);

    SessionConfig config;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(metadata.saveCalled);
    CHECK_FALSE(board.startLoggingCalled);
}

int main() {
    test_record_start_connects_and_initializes_board();
    test_record_start_reads_device_info_and_saves_metadata();
    test_record_start_configures_accel_and_gyro();
    test_record_start_erases_existing_logs_when_requested();
    test_record_start_does_not_erase_existing_logs_by_default();
    test_record_start_starts_logging_after_metadata_save();
    test_record_start_disconnects_on_success();
    test_record_start_does_not_start_logging_if_connect_fails();
    test_record_start_does_not_start_logging_if_logger_creation_fails();
    test_record_start_does_not_start_logging_if_metadata_save_fails();

    std::cout << "[PASS] RecordStartService tests\n";
    return 0;
}
