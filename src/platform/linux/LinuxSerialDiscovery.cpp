#include "LinuxSerialDiscovery.hpp"

#include <algorithm>
#include <filesystem>
#include <glob.h>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace headmotion::platform::linux_platform {

namespace {

std::vector<std::string> globPaths(const std::string& pattern) {
    glob_t result{};

    std::vector<std::string> paths;

    const int rc = glob(pattern.c_str(), 0, nullptr, &result);

    if (rc == 0) {
        for (std::size_t i = 0; i < result.gl_pathc; ++i) {
            paths.emplace_back(result.gl_pathv[i]);
        }
    }

    globfree(&result);

    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string lowerCopy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value;
}

} // namespace

std::vector<headmotion::transport::SerialPortInfo> LinuxSerialDiscovery::listPorts() const {
    std::vector<headmotion::transport::SerialPortInfo> out;

    auto by_id = listByIdDevices();
    auto globbed = listGlobbedDevices();

    std::set<std::string> seen_paths;

    for (auto& info : by_id) {
        const auto key = canonicalPath(info.path);

        if (seen_paths.insert(key).second) {
            out.push_back(std::move(info));
        }
    }

    for (auto& info : globbed) {
        const auto key = canonicalPath(info.path);

        if (seen_paths.insert(key).second) {
            out.push_back(std::move(info));
        }
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const auto& a, const auto& b) {
            if (a.likely_mms != b.likely_mms) {
                return a.likely_mms > b.likely_mms;
            }

            return a.path < b.path;
        }
    );

    return out;
}

std::vector<headmotion::transport::SerialPortInfo> LinuxSerialDiscovery::listGlobbedDevices() {
    std::vector<headmotion::transport::SerialPortInfo> out;

    const std::vector<std::string> patterns = {
        "/dev/ttyACM*",
        "/dev/ttyUSB*"
    };

    for (const auto& pattern : patterns) {
        for (const auto& path : globPaths(pattern)) {
            headmotion::transport::SerialPortInfo info;
            info.path = path;
            info.display_name = path;
            info.symlink_path.clear();
            info.likely_mms = looksLikeMmsName(path);

            out.push_back(std::move(info));
        }
    }

    return out;
}

std::vector<headmotion::transport::SerialPortInfo> LinuxSerialDiscovery::listByIdDevices() {
    std::vector<headmotion::transport::SerialPortInfo> out;

    const fs::path by_id_dir{"/dev/serial/by-id"};

    if (!fs::exists(by_id_dir) || !fs::is_directory(by_id_dir)) {
        return out;
    }

    for (const auto& entry : fs::directory_iterator(by_id_dir)) {
        const auto symlink_path = entry.path().string();

        std::error_code ec;
        const auto target = fs::canonical(entry.path(), ec);

        if (ec) {
            continue;
        }

        headmotion::transport::SerialPortInfo info;
        info.path = target.string();
        info.display_name = entry.path().filename().string();
        info.symlink_path = symlink_path;

        const std::string combined = info.path + " " + info.display_name + " " + info.symlink_path;
        info.likely_mms = looksLikeMmsName(combined);

        out.push_back(std::move(info));
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const auto& a, const auto& b) {
            return a.display_name < b.display_name;
        }
    );

    return out;
}

bool LinuxSerialDiscovery::looksLikeMmsName(const std::string& value) {
    const auto lower = lowerCopy(value);

    return lower.find("mbient") != std::string::npos ||
           lower.find("metamotion") != std::string::npos ||
           lower.find("metawear") != std::string::npos ||
           lower.find("mms") != std::string::npos ||
           lower.find("f9cb9404c345") != std::string::npos;
}

std::string LinuxSerialDiscovery::canonicalPath(const std::string& path) {
    std::error_code ec;
    const auto canonical = fs::canonical(path, ec);

    if (ec) {
        return path;
    }

    return canonical.string();
}

} // namespace headmotion::platform::linux_platform