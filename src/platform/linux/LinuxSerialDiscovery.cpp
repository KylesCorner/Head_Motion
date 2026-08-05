#include "LinuxSerialDiscovery.hpp"

#include <algorithm>
#include <filesystem>
#include <glob.h>
#include <set>
#include <string>
#include <vector>
#include <charconv>
#include <fstream>
#include <optional>
#include <system_error>

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

bool LinuxSerialDiscovery::looksLikeMmsName(
    const std::string& value
) {
    const auto lower = lowerCopy(value);

    return lower.find("mbientlab") != std::string::npos ||
           lower.find("metamotions") != std::string::npos ||
           lower.find("metawear") != std::string::npos;
}

std::string LinuxSerialDiscovery::canonicalPath(const std::string& path) {
    std::error_code ec;
    const auto canonical = fs::canonical(path, ec);

    if (ec) {
        return path;
    }

    return canonical.string();
}
std::optional<std::string> readTextFile(const fs::path& path) {
    std::ifstream in(path);

    if (!in) {
        return std::nullopt;
    }

    std::string value;
    std::getline(in, value);

    if (value.empty()) {
        return std::nullopt;
    }

    return value;
}

std::optional<std::string> findUsbAttribute(
    fs::path current,
    const char* attribute
) {
    std::error_code ec;
    current = fs::canonical(current, ec);

    if (ec) {
        return std::nullopt;
    }

    while (!current.empty() && current != current.root_path()) {
        if (const auto value = readTextFile(current / attribute)) {
            return value;
        }

        current = current.parent_path();
    }

    return std::nullopt;
}

std::uint16_t parseHex16(const std::optional<std::string>& text) {
    if (!text || text->empty()) {
        return 0;
    }

    std::uint16_t value = 0;

    const char* begin = text->data();
    const char* end = begin + text->size();

    const auto [ptr, ec] =
        std::from_chars(begin, end, value, 16);

    if (ec != std::errc{} || ptr != end) {
        return 0;
    }

    return value;
}

void populateUsbMetadata(
    headmotion::transport::SerialPortInfo& info
) {
    const fs::path tty_name =
        fs::path(info.path).filename();

    const fs::path sysfs_device =
        fs::path("/sys/class/tty") / tty_name / "device";

    info.vendor_id =
        parseHex16(findUsbAttribute(sysfs_device, "idVendor"));

    info.product_id =
        parseHex16(findUsbAttribute(sysfs_device, "idProduct"));

    info.serial_number =
        findUsbAttribute(sysfs_device, "serial").value_or("");

    info.manufacturer =
        findUsbAttribute(sysfs_device, "manufacturer").value_or("");

    info.product_name =
        findUsbAttribute(sysfs_device, "product").value_or("");

    constexpr std::uint16_t mms_vendor_id = 0x1915;
    constexpr std::uint16_t mms_product_id = 0xd978;

    const bool exact_usb_match =
        info.vendor_id == mms_vendor_id &&
        info.product_id == mms_product_id;

    const std::string combined =
        info.display_name + " " +
        info.symlink_path + " " +
        info.manufacturer + " " +
        info.product_name;

    info.likely_mms =
        exact_usb_match ||
        LinuxSerialDiscovery::looksLikeMmsName(combined);
}

} // namespace headmotion::platform::linux_platform