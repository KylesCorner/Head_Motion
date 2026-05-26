#include "headmotion/storage/FileMetadataStore.h"

#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace headmotion {

FileMetadataStore::FileMetadataStore(std::filesystem::path rootDir)
    : rootDir_(std::move(rootDir)) {}

Result FileMetadataStore::save(const SessionMetadata& metadata) {
    try {
        const std::filesystem::path deviceDir = deviceDirFor(metadata);
        std::filesystem::create_directories(deviceDir);

        const std::string fileName =
            metadata.sessionId.empty() ? "latest.session.json"
                                       : metadata.sessionId + ".session.json";

        const std::filesystem::path outPath = deviceDir / fileName;

        std::ofstream out(outPath);
        if (!out) {
            return Result::error("Failed to open metadata file: " + outPath.string());
        }

        out << toJson(metadata);
        out.close();

        if (!out) {
            return Result::error("Failed to write metadata file: " + outPath.string());
        }

        return Result::success();
    } catch (const std::exception& e) {
        return Result::error(std::string("Failed to save metadata: ") + e.what());
    }
}

std::optional<SessionMetadata>
FileMetadataStore::loadLatestForDevice(const std::string& deviceMacOrSerial) {
    try {
        std::vector<std::filesystem::path> searchDirs;

        const std::filesystem::path exactDir = rootDir_ / sanitize(deviceMacOrSerial);
        if (std::filesystem::exists(exactDir) && std::filesystem::is_directory(exactDir)) {
            searchDirs.push_back(exactDir);
        }

        // Fallback: target may be "MetaWear", while metadata is stored under serial number.
        if (searchDirs.empty() && std::filesystem::exists(rootDir_)) {
            for (const auto& entry : std::filesystem::directory_iterator(rootDir_)) {
                if (entry.is_directory()) {
                    searchDirs.push_back(entry.path());
                }
            }
        }

        std::filesystem::path newestPath;
        std::filesystem::file_time_type newestTime{};
        bool found = false;

        for (const auto& dir : searchDirs) {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                const auto path = entry.path();
                const std::string name = path.filename().string();
                const std::string suffix = ".session.json";

                if (name.size() < suffix.size()) {
                    continue;
                }

                if (name.rfind(suffix) != name.size() - suffix.size()) {
                    continue;
                }

                const auto t = std::filesystem::last_write_time(path);
                if (!found || t > newestTime) {
                    newestTime = t;
                    newestPath = path;
                    found = true;
                }
            }
        }

        if (!found) {
            return std::nullopt;
        }

        std::ifstream in(newestPath);
        if (!in) {
            return std::nullopt;
        }

        std::ostringstream buffer;
        buffer << in.rdbuf();

        return fromJson(buffer.str());
    } catch (...) {
        return std::nullopt;
    }
}

Result FileMetadataStore::markDownloaded(const std::string& sessionId) {
    try {
        if (!std::filesystem::exists(rootDir_)) {
            return Result::success();
        }

        for (const auto& deviceEntry : std::filesystem::directory_iterator(rootDir_)) {
            if (!deviceEntry.is_directory()) {
                continue;
            }

            for (const auto& entry : std::filesystem::directory_iterator(deviceEntry.path())) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                const std::filesystem::path path = entry.path();
                if (path.filename().string() != sessionId + ".session.json") {
                    continue;
                }

                std::ifstream in(path);
                if (!in) {
                    return Result::error("Failed to open metadata file for markDownloaded");
                }

                std::ostringstream buffer;
                buffer << in.rdbuf();

                std::string json = buffer.str();
                const auto insertPos = json.rfind("\n}");
                if (insertPos == std::string::npos) {
                    return Result::success();
                }

                if (json.find("\"downloaded\"") == std::string::npos) {
                    json.insert(insertPos, ",\n  \"downloaded\": true");
                }

                std::ofstream out(path);
                if (!out) {
                    return Result::error("Failed to rewrite metadata file for markDownloaded");
                }

                out << json;
                return Result::success();
            }
        }

        return Result::success();
    } catch (const std::exception& e) {
        return Result::error(std::string("markDownloaded failed: ") + e.what());
    }
}

Result FileMetadataStore::saveBoardState(const SessionMetadata& metadata,
                                         const std::vector<uint8_t>& state) {
    try {
        if (metadata.sessionId.empty()) {
            return Result::error("Cannot save board state without sessionId");
        }

        if (state.empty()) {
            return Result::error("Cannot save empty board state");
        }

        const std::filesystem::path deviceDir = deviceDirFor(metadata);
        std::filesystem::create_directories(deviceDir);

        const std::filesystem::path path = boardStatePathFor(metadata);

        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return Result::error("Failed to open board state file: " + path.string());
        }

        out.write(reinterpret_cast<const char*>(state.data()),
                  static_cast<std::streamsize>(state.size()));
        out.close();

        if (!out) {
            return Result::error("Failed to write board state file: " + path.string());
        }

        return Result::success();
    } catch (const std::exception& e) {
        return Result::error(std::string("Failed to save board state: ") + e.what());
    }
}

std::optional<std::vector<uint8_t>>
FileMetadataStore::loadBoardState(const SessionMetadata& metadata) {
    try {
        const std::filesystem::path path = boardStatePathFor(metadata);

        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }

        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );

        if (bytes.empty()) {
            return std::nullopt;
        }

        return bytes;
    } catch (...) {
        return std::nullopt;
    }
}

std::filesystem::path FileMetadataStore::deviceDirFor(const SessionMetadata& metadata) const {
    return rootDir_ / sanitize(
        metadata.serialNumber.empty() ? metadata.deviceMac : metadata.serialNumber
    );
}

std::filesystem::path FileMetadataStore::boardStatePathFor(const SessionMetadata& metadata) const {
    return deviceDirFor(metadata) / (metadata.sessionId + ".boardstate.bin");
}

std::string FileMetadataStore::sanitize(std::string value) {
    for (char& c : value) {
        const bool valid =
            std::isalnum(static_cast<unsigned char>(c)) ||
            c == '_' ||
            c == '-' ||
            c == '.';

        if (!valid) {
            c = '_';
        }
    }

    return value.empty() ? "unknown_device" : value;
}

std::string FileMetadataStore::toJson(const SessionMetadata& metadata) {
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

std::optional<std::string>
FileMetadataStore::extractString(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const auto keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const auto colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    const auto firstQuote = json.find('"', colonPos + 1);
    if (firstQuote == std::string::npos) {
        return std::nullopt;
    }

    const auto secondQuote = json.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) {
        return std::nullopt;
    }

    return json.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

std::optional<int64_t>
FileMetadataStore::extractInt64(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const auto keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const auto colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    const auto start = json.find_first_of("-0123456789", colonPos + 1);
    if (start == std::string::npos) {
        return std::nullopt;
    }

    const auto end = json.find_first_not_of("-0123456789", start);
    return std::stoll(json.substr(start, end - start));
}

std::optional<double>
FileMetadataStore::extractDouble(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const auto keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const auto colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) {
        return std::nullopt;
    }

    const auto start = json.find_first_of("-0123456789.", colonPos + 1);
    if (start == std::string::npos) {
        return std::nullopt;
    }

    const auto end = json.find_first_not_of("-0123456789.", start);
    return std::stod(json.substr(start, end - start));
}

std::optional<SessionMetadata>
FileMetadataStore::fromJson(const std::string& json) {
    SessionMetadata metadata;

    metadata.sessionId = extractString(json, "sessionId").value_or("");
    metadata.deviceMac = extractString(json, "deviceMac").value_or("");
    metadata.manufacturer = extractString(json, "manufacturer").value_or("");
    metadata.modelNumber = extractString(json, "modelNumber").value_or("");
    metadata.serialNumber = extractString(json, "serialNumber").value_or("");
    metadata.firmwareRevision = extractString(json, "firmwareRevision").value_or("");
    metadata.hardwareRevision = extractString(json, "hardwareRevision").value_or("");

    metadata.accelLoggerId = static_cast<uint8_t>(
        extractInt64(json, "accelLoggerId").value_or(0xff)
    );

    metadata.gyroLoggerId = static_cast<uint8_t>(
        extractInt64(json, "gyroLoggerId").value_or(0xff)
    );

    metadata.accelHz = static_cast<float>(
        extractDouble(json, "accelHz").value_or(50.0)
    );

    metadata.gyroHz = static_cast<float>(
        extractDouble(json, "gyroHz").value_or(50.0)
    );

    metadata.startedEpochMs = extractInt64(json, "startedEpochMs").value_or(0);

    if (metadata.sessionId.empty()) {
        return std::nullopt;
    }

    return metadata;
}

}  // namespace headmotion
