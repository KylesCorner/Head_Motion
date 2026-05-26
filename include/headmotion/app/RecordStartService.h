#pragma once

#include "headmotion/core/Result.h"
#include "headmotion/core/SessionConfig.h"
#include "headmotion/core/SessionMetadata.h"
#include "headmotion/interfaces/ILogger.h"
#include "headmotion/interfaces/IMetaWearBoard.h"
#include "headmotion/interfaces/IMetadataStore.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace headmotion {

class RecordStartService {
public:
    RecordStartService(IMetaWearBoard& board,
                       IMetadataStore& metadataStore,
                       ILogger& logger)
        : board_(board),
          metadataStore_(metadataStore),
          logger_(logger) {}

    Result run(const std::string& target, const SessionConfig& config) {
        Result r = board_.connect(target);
        if (!r.ok()) {
            logger_.error("Failed to connect: " + r.message());
            return r;
        }

        r = board_.initialize();
        if (!r.ok()) {
            logger_.error("Failed to initialize board: " + r.message());
            board_.disconnect();
            return r;
        }

        if (!board_.isInitialized()) {
            board_.disconnect();
            return Result::error("Board failed to initialize");
        }

        SessionMetadata metadata;

        r = board_.readDeviceInfo(metadata);
        if (!r.ok()) {
            logger_.error("Failed to read device info: " + r.message());
            board_.disconnect();
            return r;
        }

        metadata.sessionId = makeSessionId(metadata);
        metadata.accelHz = config.accelHz;
        metadata.gyroHz = config.gyroHz;
        metadata.startedEpochMs = nowEpochMs();

        logger_.warn("Tearing down existing MetaWear routes/loggers before record-start");

        r = board_.tearDownBoard();
        if (!r.ok()) {
            logger_.error("Failed to tear down existing routes/loggers: " + r.message());
            board_.disconnect();
            return r;
        }

        if (config.eraseBeforeStart) {
            logger_.warn("Clearing existing device log entries because --erase was requested");

            r = board_.clearLogEntries();
            if (!r.ok()) {
                logger_.error("Failed to clear existing log entries: " + r.message());
                board_.disconnect();
                return r;
            }
        }

        r = board_.configureHeadMotionSignals(config);
        if (!r.ok()) {
            logger_.error("Failed to configure head motion signals: " + r.message());
            board_.disconnect();
            return r;
        }

        r = board_.createHeadMotionLoggers(metadata);
        if (!r.ok()) {
            logger_.error("Failed to create head motion loggers: " + r.message());
            board_.disconnect();
            return r;
        }

        if (metadata.accelLoggerId == 0xff || metadata.gyroLoggerId == 0xff) {
            board_.disconnect();
            return Result::error("Logger creation did not assign valid logger IDs");
        }

        r = metadataStore_.save(metadata);
        if (!r.ok()) {
            logger_.error("Failed to save session metadata: " + r.message());
            board_.disconnect();
            return r;
        }

        std::vector<uint8_t> boardState;
        r = board_.serializeBoardState(boardState);
        if (!r.ok()) {
            logger_.error("Failed to serialize MetaWear board state: " + r.message());
            board_.disconnect();
            return r;
        }

        r = metadataStore_.saveBoardState(metadata, boardState);
        if (!r.ok()) {
            logger_.error("Failed to save MetaWear board state: " + r.message());
            board_.disconnect();
            return r;
        }

        r = board_.startLogging(/*overwrite=*/true);
        if (!r.ok()) {
            logger_.error("Failed to start logging: " + r.message());
            board_.disconnect();
            return r;
        }

        board_.disconnect();

        logger_.info("HeadMotion logging started: " + metadata.sessionId);
        return Result::success();
    }

private:
    static int64_t nowEpochMs() {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        );
        return ms.count();
    }

    static std::string timestampString() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);

        std::tm tm{};

#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d_%H%M%S");
        return oss.str();
    }

    static std::string makeSessionId(const SessionMetadata& metadata) {
        const std::string deviceId =
            metadata.serialNumber.empty() ? "unknown_device" : metadata.serialNumber;

        return deviceId + "_" + timestampString();
    }

    IMetaWearBoard& board_;
    IMetadataStore& metadataStore_;
    ILogger& logger_;
};

}  // namespace headmotion
