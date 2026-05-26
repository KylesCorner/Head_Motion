#pragma once

#include "headmotion/interfaces/IMetaWearBoard.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <simpleble/SimpleBLE.h>

extern "C" {
#include "metawear/core/data.h"
#include "metawear/core/datasignal.h"
#include "metawear/core/logging.h"
#include "metawear/core/logging_fwd.h"
#include "metawear/core/metawearboard.h"
#include "metawear/core/module.h"
#include "metawear/core/types.h"
#include "metawear/platform/btle_connection.h"
#include "metawear/sensor/accelerometer.h"
#include "metawear/sensor/gyro_bosch.h"
}

namespace headmotion {

class MetaWearBoardAdapter : public IMetaWearBoard {
public:
    MetaWearBoardAdapter();
    ~MetaWearBoardAdapter() override;

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
    Result tearDownBoard() override;

    Result serializeBoardState(std::vector<uint8_t>& stateOut) override;
    Result deserializeBoardState(const std::vector<uint8_t>& state) override;

private:
    enum class GyroImpl {
        Unknown,
        Bmi160,
        Bmi270
    };

    struct InitWaiter {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        int32_t status = -1;
    };

    struct LoggerWaiter {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        MblMwDataLogger* logger = nullptr;
    };

    struct LoggerCallbackContext {
        MetaWearBoardAdapter* self = nullptr;
        ImuSample::Kind kind = ImuSample::Kind::Accel;
    };

    static bool targetMatches(SimpleBLE::Peripheral& peripheral,
                              const std::string& target);

    static std::string lower(std::string value);

    static void writeGattChar(void* context,
                              const void* caller,
                              MblMwGattCharWriteType writeType,
                              const MblMwGattChar* characteristic,
                              const uint8_t* value,
                              uint8_t length);

    static void readGattChar(void* context,
                             const void* caller,
                             const MblMwGattChar* characteristic,
                             MblMwFnIntVoidPtrArray handler);

    static void enableNotifications(void* context,
                                    const void* caller,
                                    const MblMwGattChar* characteristic,
                                    MblMwFnIntVoidPtrArray handler,
                                    MblMwFnVoidVoidPtrInt ready);

    static void onDisconnect(void* context,
                             const void* caller,
                             MblMwFnVoidVoidPtrInt handler);

    static void handleLoggerData(void* context, const MblMwData* data);

    Result waitForLogger(LoggerWaiter& waiter,
                         const std::string& name,
                         MblMwDataLogger*& outLogger);

    Result detectGyroImpl();
    Result configureGyro(const SessionConfig& config);
    MblMwDataSignal* getGyroSignal();
    Result startGyro();
    Result stopGyro();

    MblMwBtleConnection connection_{};
    MblMwMetaWearBoard* board_ = nullptr;

    std::optional<SimpleBLE::Peripheral> peripheral_;
    std::string connectedAddress_;

    bool initialized_ = false;

    GyroImpl gyroImpl_ = GyroImpl::Unknown;

    MblMwDataSignal* accelSignal_ = nullptr;
    MblMwDataSignal* gyroSignal_ = nullptr;

    MblMwDataLogger* accelLogger_ = nullptr;
    MblMwDataLogger* gyroLogger_ = nullptr;

    std::function<void(const ImuSample&)> onSample_;

    LoggerCallbackContext accelCallbackCtx_{this, ImuSample::Kind::Accel};
    LoggerCallbackContext gyroCallbackCtx_{this, ImuSample::Kind::Gyro};
};

}  // namespace headmotion
