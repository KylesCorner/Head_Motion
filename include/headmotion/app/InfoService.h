#pragma once

#include "headmotion/core/Result.h"
#include "headmotion/core/SessionMetadata.h"
#include "headmotion/interfaces/ILogger.h"
#include "headmotion/interfaces/IMetaWearBoard.h"

#include <string>

namespace headmotion {

class InfoService {
public:
    InfoService(IMetaWearBoard& board, ILogger& logger)
        : board_(board),
          logger_(logger) {}

    Result run(const std::string& target, SessionMetadata& metadataOut) {
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

        r = board_.readDeviceInfo(metadataOut);
        if (!r.ok()) {
            logger_.error("Failed to read device info: " + r.message());
            board_.disconnect();
            return r;
        }

        board_.disconnect();

        logger_.info("Read device info for " + target);
        return Result::success();
    }

private:
    IMetaWearBoard& board_;
    ILogger& logger_;
};

}  // namespace headmotion
