#pragma once

#include "headmotion/core/Result.h"
#include "headmotion/core/SessionMetadata.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace headmotion {

class IMetadataStore {
public:
    virtual ~IMetadataStore() = default;

    virtual Result save(const SessionMetadata& metadata) = 0;

    virtual std::optional<SessionMetadata>
    loadLatestForDevice(const std::string& deviceMacOrSerial) = 0;

    virtual Result markDownloaded(const std::string& sessionId) = 0;

    virtual Result saveBoardState(const SessionMetadata&, const std::vector<uint8_t>&) {
        return Result::success();
    }

    virtual std::optional<std::vector<uint8_t>>
    loadBoardState(const SessionMetadata&) {
        return std::nullopt;
    }
};

}  // namespace headmotion
