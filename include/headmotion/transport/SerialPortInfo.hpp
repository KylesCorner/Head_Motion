#pragma once

#include <cstdint>
#include <string>

namespace headmotion::transport {

struct SerialPortInfo {
    // Current operating-system device path:
    //   Linux:   /dev/ttyACM0
    //   Windows: COM3
    std::string path;

    std::string display_name;

    // Stable alias when the platform provides one:
    //   Linux: /dev/serial/by-id/usb-MbientLab_...
    std::string symlink_path;

    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;

    std::string serial_number;
    std::string manufacturer;
    std::string product_name;

    bool likely_mms = false;

    [[nodiscard]]
    std::string preferredPath() const {
        return symlink_path.empty() ? path : symlink_path;
    }
};

} // namespace headmotion::transport