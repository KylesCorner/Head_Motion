#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace headmotion::session {

struct SavedDevicePort {
    std::string preferred_path;
    std::string system_path;

    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;

    std::string serial_number;
};

class DevicePortStore {
public:
    static std::string defaultPath();

    static void save(
        const std::string& path,
        const SavedDevicePort& port
    );

    static std::optional<SavedDevicePort> load(
        const std::string& path
    );
};

} // namespace headmotion::session