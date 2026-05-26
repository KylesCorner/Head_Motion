#include "headmotion/storage/JsonMetadataStore.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace headmotion {

JsonMetadataStore::JsonMetadataStore(std::filesystem::path rootDir)
    : rootDir_(std::move(rootDir)) {}

Result JsonMetadataStore::save(const SessionMetadata& metadata) {
    try {
        const std::string deviceDirName = sanitizePathComponent(
            metadata.serialNumber.empty() ? metadata.deviceMac : metadata.serialNumber
        );

        const std::filesystem::path deviceDir = rootDir_ / deviceDirName;
        std::filesystem::create_directories(deviceDir);

        const std::filesystem::path path = deviceDir / (metadata.sessionId + ".session.json");

        std::ofstream out(path);
        if (!out) {
            return Result::error("Failed to open metadata file for writing: " + path.string());
        }

        out << metadataToJson(metadata);
        out.close();

        if (!out) {
            return Result::error("Failed to write metadata file: " + path.string());
        }

        return Result::success();
    } catch (const std::exception& e) {
        return Result::error(std::string("Metadata save failed: ") + e.what());
    }
}

std::optional<SessionMetadata>
JsonMetadataStore::loadLatestForDevice(const std::string&) {
    // This will be implemented during sync.
    return std::nullopt;
}

Result JsonMetadataStore::markDownloaded(const std::string&) {
    // This will be implemented during sync.
    return Result::success();
}

std::string JsonMetadataStore::sanitizePathComponent(std::string value) {
    for (char& c : value) {
        const bool ok =
            std::isalnum(static_cast<unsigned char>(c)) ||
            c == '_' ||
            c == '-' ||
            c == '.';

        if (!ok) {
            c = '_';
        }
    }

    if (value.empty()) {
        return "unknown_device";
    }

    return value;
}

std::string JsonMetadataStore::metadataToJson(const SessionMetadata& metadata) {
    std::ostringstream oss;

    oss << "{\n";
    oss << "  \"sessionId\": \"" << metadata.sessionId << "\",\n";
    oss << "  \"deviceMac\": \"" << metadata.deviceMac << "\",\n";
    oss << "  \"manufacturer\": \"" << metadata.manufacturer << "\",\n";
    oss << "  \"modelNumber\": \"" << metadata.modelNumber << "\",\n";
    oss << "  \"serialNumber\": \"" << metadata.serialNumber << "\",\n";
    oss << "  \"firmwareRevision\": \"" << metadata.firmwareRevision << "\",\n";
    oss << "  \"hardwareRevision\": \"" << metadata.hardwareRevision << "\",\n";
    oss << "  \"accelLoggerId\": " << static_cast<int>(metadata.accelLoggerId) << ",\n";
    oss << "  \"gyroLoggerId\": " << static_cast<int>(metadata.gyroLoggerId) << ",\n";
    oss << "  \"accelHz\": " << metadata.accelHz << ",\n";
    oss << "  \"gyroHz\": " << metadata.gyroHz << ",\n";
    oss << "  \"startedEpochMs\": " << metadata.startedEpochMs << "\n";
    oss << "}\n";

    return oss.str();
}

}  // namespace headmotion
