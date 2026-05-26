#pragma once

#include "headmotion/interfaces/IMetadataStore.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace headmotion {

class FileMetadataStore : public IMetadataStore {
public:
    explicit FileMetadataStore(std::filesystem::path rootDir);

    Result save(const SessionMetadata& metadata) override;

    std::optional<SessionMetadata>
    loadLatestForDevice(const std::string& deviceMacOrSerial) override;

    Result markDownloaded(const std::string& sessionId) override;

    Result saveBoardState(const SessionMetadata& metadata,
                          const std::vector<uint8_t>& state) override;

    std::optional<std::vector<uint8_t>>
    loadBoardState(const SessionMetadata& metadata) override;

    std::filesystem::path rootDir() const {
        return rootDir_;
    }

private:
    std::filesystem::path rootDir_;

    static std::string sanitize(std::string value);
    static std::string toJson(const SessionMetadata& metadata);
    static std::optional<std::string> extractString(const std::string& json,
                                                    const std::string& key);
    static std::optional<int64_t> extractInt64(const std::string& json,
                                               const std::string& key);
    static std::optional<double> extractDouble(const std::string& json,
                                               const std::string& key);
    static std::optional<SessionMetadata> fromJson(const std::string& json);

    std::filesystem::path deviceDirFor(const SessionMetadata& metadata) const;
    std::filesystem::path boardStatePathFor(const SessionMetadata& metadata) const;
};

}  // namespace headmotion
