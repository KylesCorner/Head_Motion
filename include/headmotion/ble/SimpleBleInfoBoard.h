#pragma once

#include "headmotion/interfaces/IMetaWearBoard.h"

#include <optional>
#include <string>

#include <simpleble/SimpleBLE.h>

namespace headmotion {

class SimpleBleInfoBoard : public IMetaWearBoard {
public:
    SimpleBleInfoBoard() = default;
    ~SimpleBleInfoBoard() override;

    Result connect(const std::string& target) override;
    Result initialize() override;
    void disconnect() override;

    bool isInitialized() const override;

    Result readDeviceInfo(SessionMetadata& metadataOut) override;

    Result configureHeadMotionSignals(const SessionConfig& config) override;
    Result createHeadMotionLoggers(SessionMetadata& metadataInOut) override;
    Result startLogging(bool overwrite) override;
    Result stopLogging() override;
    Result flushLogPage() override;

    Result subscribeLoggers(
        const SessionMetadata& metadata,
        std::function<void(const ImuSample&)> onSample
    ) override;

    Result downloadLogs(
        std::function<void(const DownloadProgress&)> onProgress
    ) override;

    Result clearLogEntries() override;

private:
    static bool targetMatches(SimpleBLE::Peripheral& peripheral,
                              const std::string& target);

    static std::string readStringCharacteristic(SimpleBLE::Peripheral& peripheral,
                                                const SimpleBLE::BluetoothUUID& serviceUuid,
                                                const SimpleBLE::BluetoothUUID& characteristicUuid);

    std::optional<SimpleBLE::Peripheral> peripheral_;
    std::string connectedAddress_;
    bool initialized_ = false;
};

}  // namespace headmotion
