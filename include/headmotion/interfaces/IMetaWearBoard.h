#pragma once

#include "headmotion/core/DownloadProgress.h"
#include "headmotion/core/HeadMotionSample.h"
#include "headmotion/core/Result.h"
#include "headmotion/core/SessionConfig.h"
#include "headmotion/core/SessionMetadata.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace headmotion {

class IMetaWearBoard {
public:
    virtual ~IMetaWearBoard() = default;

    virtual Result connect(const std::string& target) = 0;
    virtual Result initialize() = 0;
    virtual void disconnect() = 0;

    virtual bool isInitialized() const = 0;

    virtual Result readDeviceInfo(SessionMetadata& metadataOut) = 0;

    virtual Result configureHeadMotionSignals(const SessionConfig& config) = 0;
    virtual Result createHeadMotionLoggers(SessionMetadata& metadataInOut) = 0;

    virtual Result startLogging(bool overwrite) = 0;
    virtual Result stopLogging() = 0;

    virtual Result flushLogPage() = 0;

    virtual Result subscribeLoggers(
        const SessionMetadata& metadata,
        std::function<void(const ImuSample&)> onSample
    ) = 0;

    virtual Result downloadLogs(
        std::function<void(const DownloadProgress&)> onProgress
    ) = 0;

    virtual Result clearLogEntries() = 0;

    virtual Result tearDownBoard() {
        return Result::success();
    }

    virtual Result serializeBoardState(std::vector<uint8_t>& stateOut) {
        stateOut.clear();
        return Result::success();
    }

    virtual Result deserializeBoardState(const std::vector<uint8_t>&) {
        return Result::success();
    }
};

}  // namespace headmotion
