#include "headmotion/app/SyncService.h"

#include "FakeMetaWearBoard.h"
#include "FakeMetadataStore.h"
#include "FakeSampleSink.h"
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

static SessionMetadata makeMetadata() {
    SessionMetadata metadata;
    metadata.sessionId = "0561E1_2026-05-18_125034";
    metadata.deviceMac = "F9:CB:94:04:C3:45";
    metadata.serialNumber = "0561E1";
    metadata.firmwareRevision = "1.7.2";
    metadata.hardwareRevision = "0.1";
    metadata.manufacturer = "MbientLab Inc";
    metadata.modelNumber = "8";
    metadata.accelLoggerId = 4;
    metadata.gyroLoggerId = 5;
    metadata.accelHz = 50.0f;
    metadata.gyroHz = 50.0f;
    return metadata;
}

static ImuSample makeAccelSample() {
    ImuSample sample;
    sample.kind = ImuSample::Kind::Accel;
    sample.epochMs = 1000;
    sample.x = 1.0f;
    sample.y = 2.0f;
    sample.z = 3.0f;
    return sample;
}

static ImuSample makeGyroSample() {
    ImuSample sample;
    sample.kind = ImuSample::Kind::Gyro;
    sample.epochMs = 1001;
    sample.x = 10.0f;
    sample.y = 20.0f;
    sample.z = 30.0f;
    return sample;
}

static void test_sync_happy_path_clears_logs_by_default() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadataStore;
    FakeSampleSink sink;
    TestLogger logger;

    metadataStore.savedMetadata = makeMetadata();

    board.samplesToEmitOnDownload.push_back(makeAccelSample());
    board.samplesToEmitOnDownload.push_back(makeGyroSample());

    SyncService service(board, metadataStore, sink, logger);

    SyncService::Config config;
    config.clearAfterSuccess = true;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());

    CHECK_TRUE(board.connectCalled);
    CHECK_TRUE(board.initializeCalled);
    CHECK_TRUE(board.readDeviceInfoCalled);
    CHECK_TRUE(board.stopLoggingCalled);
    CHECK_TRUE(board.flushLogPageCalled);
    CHECK_TRUE(sink.openCalled);
    CHECK_TRUE(board.subscribeLoggersCalled);
    CHECK_TRUE(board.downloadLogsCalled);
    CHECK_TRUE(sink.closeAndCommitCalled);
    CHECK_TRUE(metadataStore.markDownloadedCalled);
    CHECK_TRUE(board.clearLogEntriesCalled);
    CHECK_TRUE(board.disconnectCalled);

    CHECK_EQ(sink.samples.size(), static_cast<size_t>(2));
    CHECK_EQ(metadataStore.markedDownloadedSessionId,
             std::string("0561E1_2026-05-18_125034"));
}

static void test_sync_no_clear_leaves_device_logs_intact() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadataStore;
    FakeSampleSink sink;
    TestLogger logger;

    metadataStore.savedMetadata = makeMetadata();
    board.samplesToEmitOnDownload.push_back(makeAccelSample());

    SyncService service(board, metadataStore, sink, logger);

    SyncService::Config config;
    config.clearAfterSuccess = false;

    Result result = service.run("MetaWear", config);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(board.downloadLogsCalled);
    CHECK_TRUE(metadataStore.markDownloadedCalled);
    CHECK_FALSE(board.clearLogEntriesCalled);
}

static void test_sync_fails_safely_if_logger_subscription_fails() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadataStore;
    FakeSampleSink sink;
    TestLogger logger;

    metadataStore.savedMetadata = makeMetadata();

    board.subscribeLoggersResult =
        Result::error("Could not look up accelerometer logger ID 4");

    SyncService service(board, metadataStore, sink, logger);

    SyncService::Config config;
    config.clearAfterSuccess = true;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());

    CHECK_TRUE(board.stopLoggingCalled);
    CHECK_TRUE(board.flushLogPageCalled);
    CHECK_TRUE(sink.openCalled);
    CHECK_TRUE(board.subscribeLoggersCalled);

    CHECK_FALSE(board.downloadLogsCalled);
    CHECK_FALSE(sink.closeAndCommitCalled);
    CHECK_FALSE(metadataStore.markDownloadedCalled);
    CHECK_FALSE(board.clearLogEntriesCalled);
    CHECK_TRUE(board.disconnectCalled);
}

static void test_sync_fails_safely_if_download_fails() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadataStore;
    FakeSampleSink sink;
    TestLogger logger;

    metadataStore.savedMetadata = makeMetadata();

    board.downloadLogsResult = Result::error("download timed out");

    SyncService service(board, metadataStore, sink, logger);

    SyncService::Config config;
    config.clearAfterSuccess = true;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());

    CHECK_TRUE(board.subscribeLoggersCalled);
    CHECK_TRUE(board.downloadLogsCalled);

    CHECK_FALSE(sink.closeAndCommitCalled);
    CHECK_FALSE(metadataStore.markDownloadedCalled);
    CHECK_FALSE(board.clearLogEntriesCalled);
    CHECK_TRUE(board.disconnectCalled);
}

static void test_sync_fails_safely_if_commit_fails() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadataStore;
    FakeSampleSink sink;
    TestLogger logger;

    metadataStore.savedMetadata = makeMetadata();
    board.samplesToEmitOnDownload.push_back(makeAccelSample());

    sink.closeAndCommitResult = Result::error("commit failed");

    SyncService service(board, metadataStore, sink, logger);

    SyncService::Config config;
    config.clearAfterSuccess = true;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());

    CHECK_TRUE(board.downloadLogsCalled);
    CHECK_TRUE(sink.closeAndCommitCalled);

    CHECK_FALSE(metadataStore.markDownloadedCalled);
    CHECK_FALSE(board.clearLogEntriesCalled);
    CHECK_TRUE(board.disconnectCalled);
}

static void test_sync_fails_if_no_samples_are_written() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadataStore;
    FakeSampleSink sink;
    TestLogger logger;

    metadataStore.savedMetadata = makeMetadata();

    SyncService service(board, metadataStore, sink, logger);

    SyncService::Config config;
    config.clearAfterSuccess = true;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());

    CHECK_TRUE(board.downloadLogsCalled);
    CHECK_TRUE(sink.closeAndCommitCalled);

    CHECK_FALSE(metadataStore.markDownloadedCalled);
    CHECK_FALSE(board.clearLogEntriesCalled);
    CHECK_TRUE(board.disconnectCalled);
}

static void test_sync_fails_if_metadata_missing() {
    FakeMetaWearBoard board;
    FakeMetadataStore metadataStore;
    FakeSampleSink sink;
    TestLogger logger;

    SyncService service(board, metadataStore, sink, logger);

    SyncService::Config config;
    config.clearAfterSuccess = true;

    Result result = service.run("MetaWear", config);

    CHECK_FALSE(result.ok());

    CHECK_TRUE(board.connectCalled);
    CHECK_TRUE(board.initializeCalled);
    CHECK_TRUE(board.readDeviceInfoCalled);

    CHECK_FALSE(board.stopLoggingCalled);
    CHECK_FALSE(board.flushLogPageCalled);
    CHECK_FALSE(board.subscribeLoggersCalled);
    CHECK_FALSE(board.downloadLogsCalled);
    CHECK_FALSE(board.clearLogEntriesCalled);
    CHECK_TRUE(board.disconnectCalled);
}

int main() {
    test_sync_happy_path_clears_logs_by_default();
    test_sync_no_clear_leaves_device_logs_intact();
    test_sync_fails_safely_if_logger_subscription_fails();
    test_sync_fails_safely_if_download_fails();
    test_sync_fails_safely_if_commit_fails();
    test_sync_fails_if_no_samples_are_written();
    test_sync_fails_if_metadata_missing();

    std::cout << "[PASS] SyncService tests\n";
    return 0;
}
