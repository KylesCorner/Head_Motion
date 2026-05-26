#pragma once

#include "headmotion/core/DownloadProgress.h"
#include "headmotion/core/Result.h"
#include "headmotion/core/SessionMetadata.h"
#include "headmotion/interfaces/ILogger.h"
#include "headmotion/interfaces/IMetaWearBoard.h"
#include "headmotion/interfaces/IMetadataStore.h"
#include "headmotion/interfaces/ISampleSink.h"

#include <optional>
#include <string>
#include <utility>

namespace headmotion {

class SyncService {
public:
    struct Config {
        bool clearAfterSuccess = true;
    };

    SyncService(IMetaWearBoard& board,
                IMetadataStore& metadataStore,
                ISampleSink& sampleSink,
                ILogger& logger)
        : board_(board),
          metadataStore_(metadataStore),
          sampleSink_(sampleSink),
          logger_(logger) {}

    Result run(const std::string& target, const Config& config) {
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

        SessionMetadata deviceInfo;
        r = board_.readDeviceInfo(deviceInfo);
        if (!r.ok()) {
            logger_.error("Failed to read device info: " + r.message());
            board_.disconnect();
            return r;
        }

        std::optional<SessionMetadata> metadataOpt;

        if (!deviceInfo.serialNumber.empty()) {
            metadataOpt = metadataStore_.loadLatestForDevice(deviceInfo.serialNumber);
        }

        if (!metadataOpt.has_value() && !deviceInfo.deviceMac.empty()) {
            metadataOpt = metadataStore_.loadLatestForDevice(deviceInfo.deviceMac);
        }

        if (!metadataOpt.has_value()) {
            board_.disconnect();
            return Result::error("No session metadata found for this device");
        }

        SessionMetadata metadata = *metadataOpt;
        logger_.info("Loaded session: " + metadata.sessionId);

        r = board_.stopLogging();
        if (!r.ok()) {
            logger_.error("Failed to stop logging: " + r.message());
            board_.disconnect();
            return r;
        }

        r = board_.flushLogPage();
        if (!r.ok()) {
            logger_.error("Failed to flush log page: " + r.message());
            board_.disconnect();
            return r;
        }

        r = sampleSink_.open(metadata);
        if (!r.ok()) {
            logger_.error("Failed to open sample sink: " + r.message());
            board_.disconnect();
            return r;
        }

        r = board_.subscribeLoggers(
            metadata,
            [this](const ImuSample& sample) {
                Result writeResult = sampleSink_.write(sample);
                if (!writeResult.ok()) {
                    logger_.error("Failed to write sample: " + writeResult.message());
                }
            }
        );

        if (!r.ok()) {
            logger_.error("Failed to subscribe loggers: " + r.message());
            board_.disconnect();
            return r;
        }

        r = board_.downloadLogs(
            [this](const DownloadProgress& progress) {
                logger_.info(
                    "Download progress: entries_left=" +
                    std::to_string(progress.entriesLeft) +
                    " total_entries=" +
                    std::to_string(progress.totalEntries)
                );
            }
        );

        if (!r.ok()) {
            logger_.error("Failed to download logs: " + r.message());
            board_.disconnect();
            return r;
        }

        r = sampleSink_.closeAndCommit();
        if (!r.ok()) {
            logger_.error("Failed to commit sample output: " + r.message());
            board_.disconnect();
            return r;
        }

        if (sampleSink_.sampleCount() == 0) {
            board_.disconnect();
            return Result::error("Download completed but no samples were written");
        }

        r = metadataStore_.markDownloaded(metadata.sessionId);
        if (!r.ok()) {
            logger_.error("Failed to mark metadata downloaded: " + r.message());
            board_.disconnect();
            return r;
        }

        if (config.clearAfterSuccess) {
            r = board_.clearLogEntries();
            if (!r.ok()) {
                logger_.error("Failed to clear device logs: " + r.message());
                board_.disconnect();
                return r;
            }
        } else {
            logger_.warn("Leaving device logs intact because clearAfterSuccess=false");
        }

        board_.disconnect();
        return Result::success();
    }

private:
    IMetaWearBoard& board_;
    IMetadataStore& metadataStore_;
    ISampleSink& sampleSink_;
    ILogger& logger_;
};

}  // namespace headmotion
