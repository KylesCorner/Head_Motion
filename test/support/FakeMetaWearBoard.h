#pragma once

#include "headmotion/interfaces/IMetaWearBoard.h"

#include <functional>
#include <string>
#include <vector>

namespace headmotion::test {

class FakeMetaWearBoard : public IMetaWearBoard {
public:
  bool connectCalled = false;
  bool initializeCalled = false;
  bool disconnectCalled = false;
  bool readDeviceInfoCalled = false;
  bool clearLogEntriesCalled = false;
  bool configureSignalsCalled = false;
  bool createLoggersCalled = false;
  bool startLoggingCalled = false;
  bool stopLoggingCalled = false;
  bool flushLogPageCalled = false;
  bool subscribeLoggersCalled = false;
  bool downloadLogsCalled = false;

  bool initialized = true;
  bool startLoggingOverwriteValue = false;

  std::string connectedTarget;

  Result connectResult = Result::success();
  Result initializeResult = Result::success();
  Result readDeviceInfoResult = Result::success();
  Result clearLogEntriesResult = Result::success();
  Result configureSignalsResult = Result::success();
  Result createLoggersResult = Result::success();
  Result startLoggingResult = Result::success();
  Result stopLoggingResult = Result::success();
  Result flushLogPageResult = Result::success();
  Result subscribeLoggersResult = Result::success();
  Result downloadLogsResult = Result::success();

  SessionConfig lastConfig;
  SessionMetadata deviceInfoToReturn;
  SessionMetadata subscribedMetadata;

  std::function<void(const ImuSample &)> subscribedCallback;
  std::vector<ImuSample> samplesToEmitOnDownload;

  bool tearDownBoardCalled = false;
  Result tearDownBoardResult = Result::success();

  Result tearDownBoard() override {
    tearDownBoardCalled = true;
    return tearDownBoardResult;
  }

  FakeMetaWearBoard() {
    deviceInfoToReturn.deviceMac = "F9:CB:94:04:C3:45";
    deviceInfoToReturn.manufacturer = "MbientLab Inc";
    deviceInfoToReturn.modelNumber = "8";
    deviceInfoToReturn.serialNumber = "0561E1";
    deviceInfoToReturn.firmwareRevision = "1.7.2";
    deviceInfoToReturn.hardwareRevision = "0.1";
  }

  Result connect(const std::string &target) override {
    connectCalled = true;
    connectedTarget = target;
    return connectResult;
  }

  Result initialize() override {
    initializeCalled = true;
    return initializeResult;
  }

  void disconnect() override { disconnectCalled = true; }

  bool isInitialized() const override { return initialized; }

  Result readDeviceInfo(SessionMetadata &metadataOut) override {
    readDeviceInfoCalled = true;

    if (!readDeviceInfoResult.ok()) {
      return readDeviceInfoResult;
    }

    metadataOut = deviceInfoToReturn;
    return Result::success();
  }

  Result configureHeadMotionSignals(const SessionConfig &config) override {
    configureSignalsCalled = true;
    lastConfig = config;
    return configureSignalsResult;
  }

  Result createHeadMotionLoggers(SessionMetadata &metadataInOut) override {
    createLoggersCalled = true;

    if (!createLoggersResult.ok()) {
      return createLoggersResult;
    }

    metadataInOut.accelLoggerId = 0;
    metadataInOut.gyroLoggerId = 1;

    return Result::success();
  }

  Result startLogging(bool overwrite) override {
    startLoggingCalled = true;
    startLoggingOverwriteValue = overwrite;
    return startLoggingResult;
  }

  Result stopLogging() override {
    stopLoggingCalled = true;
    return stopLoggingResult;
  }

  Result flushLogPage() override {
    flushLogPageCalled = true;
    return flushLogPageResult;
  }

  Result
  subscribeLoggers(const SessionMetadata &metadata,
                   std::function<void(const ImuSample &)> onSample) override {
    subscribeLoggersCalled = true;
    subscribedMetadata = metadata;

    if (!subscribeLoggersResult.ok()) {
      return subscribeLoggersResult;
    }

    subscribedCallback = std::move(onSample);
    return Result::success();
  }

  Result downloadLogs(
      std::function<void(const DownloadProgress &)> onProgress) override {
    downloadLogsCalled = true;

    if (!downloadLogsResult.ok()) {
      return downloadLogsResult;
    }

    if (subscribedCallback) {
      for (const auto &sample : samplesToEmitOnDownload) {
        subscribedCallback(sample);
      }
    }

    if (onProgress) {
      DownloadProgress progress;
      progress.totalEntries =
          static_cast<uint32_t>(samplesToEmitOnDownload.size());
      progress.entriesLeft = 0;
      onProgress(progress);
    }

    return Result::success();
  }

  Result clearLogEntries() override {
    clearLogEntriesCalled = true;
    return clearLogEntriesResult;
  }
};

} // namespace headmotion::test
