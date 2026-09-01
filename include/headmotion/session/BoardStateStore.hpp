#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace headmotion::session {

class BoardStateStore {
public:
    static std::filesystem::path configRoot();

    static std::string deviceIdForPort(
        const std::string& port_name
    );

    static std::filesystem::path deviceDirectory(
        const std::string& device_id
    );

    static std::filesystem::path boardStatePath(
        const std::string& device_id
    );

    static std::filesystem::path loggerMetadataPath(
        const std::string& device_id
    );

    static void save(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes
    );

    static std::vector<std::uint8_t> load(
        const std::filesystem::path& path
    );
};

} // namespace headmotion::session