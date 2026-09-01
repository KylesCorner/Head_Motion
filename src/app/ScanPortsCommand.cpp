#include "headmotion/app/MmsDeviceProbe.hpp"
#include "headmotion/session/DevicePortStore.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint16_t MMS_VENDOR_ID = 0x1915;
constexpr std::uint16_t MMS_PRODUCT_ID = 0xd978;

bool exactMmsUsbMatch(
    const headmotion::transport::SerialPortInfo& port
) {
    return
        port.vendor_id == MMS_VENDOR_ID &&
        port.product_id == MMS_PRODUCT_ID;
}

struct VerifiedMms {
    headmotion::transport::SerialPortInfo port;
    std::string identity;
    std::string device_id;
};

std::string stableDeviceId(
    const headmotion::transport::SerialPortInfo& port
) {
    /*
     * Preferred source: serial metadata supplied directly by the
     * serial-port backend.
     */
    if (!port.serial_number.empty()) {
        return port.serial_number;
    }

    /*
     * Linux fallback:
     *
     * Example:
     *
     * /dev/serial/by-id/
     * usb-MbientLab_MetaMotionS_D55AED28027D-if00
     *
     * Extract:
     *
     * D55AED28027D
     */
    if (!port.symlink_path.empty()) {
        const std::string marker =
            "usb-MbientLab_MetaMotionS_";

        const std::size_t begin =
            port.symlink_path.find(marker);

        if (begin != std::string::npos) {
            const std::size_t serial_begin =
                begin + marker.size();

            const std::size_t serial_end =
                port.symlink_path.find(
                    "-if",
                    serial_begin
                );

            if (
                serial_end != std::string::npos &&
                serial_end > serial_begin
            ) {
                return port.symlink_path.substr(
                    serial_begin,
                    serial_end - serial_begin
                );
            }
        }
    }

    return {};
}


} // namespace

namespace headmotion::app {

int runScanPortsCommand() {
    const auto ports =
        headmotion::transport::SerialPortFactory::
            listPorts();

    if (ports.empty()) {
        std::cerr
            << "No serial ports found.\n";

        return 2;
    }

    std::vector<VerifiedMms> verified_devices;

    std::cout
        << "Scanning serial ports:\n";

    for (const auto& port : ports) {
        std::cout
            << "  "
            << port.path;

        if (!port.symlink_path.empty()) {
            std::cout
                << " via "
                << port.symlink_path;
        }

        if (
            port.vendor_id != 0 ||
            port.product_id != 0
        ) {
            std::cout
                << " [USB "
                << std::hex
                << std::setw(4)
                << std::setfill('0')
                << port.vendor_id
                << ":"
                << std::setw(4)
                << port.product_id
                << std::dec
                << "]";
        }

        if (!port.serial_number.empty()) {
            std::cout
                << " serial="
                << port.serial_number;
        }

        const bool candidate =
            exactMmsUsbMatch(port) ||
            port.likely_mms;

        if (!candidate) {
            std::cout << "\n";
            continue;
        }

        std::cout
            << " MMS candidate";

        const auto probe =
            probeMmsDevice(
                port.preferredPath()
            );

        if (!probe) {
            std::cout
                << " - protocol verification failed\n";

            continue;
        }

        /*
         * Multi-device state requires a stable physical identity.
         *
         * /dev/ttyACM0, /dev/ttyACM1, COM3, etc. are not suitable
         * because they may change after reconnecting the sensors.
         */

         const std::string device_id =
            stableDeviceId(port);

        if (device_id.empty()) {
            std::cout
                << " - verified, but no stable device ID could be determined\n";

            std::cerr
                << "Cannot register MMS+ at "
                << port.preferredPath()
                << " because no stable hardware identity is available.\n";

            continue;
        }

    std::cout
        << " - verified: "
        << probe->identity
        << " device_id="
        << device_id
        << "\n";

        std::cout
            << " - verified: "
            << probe->identity
            << "\n";

        verified_devices.push_back({
            .port = port,
            .identity = probe->identity
        });
    }

    if (verified_devices.empty()) {
        std::cerr
            << "No verified MMS+ devices found.\n";

        return 3;
    }

    std::cout
        << "\nVerified MMS+ devices: "
        << verified_devices.size()
        << "\n";

    /*
     * Save every verified device independently.
     *
     * There is no longer a single global/default MMS+.
     */
    for (const auto& device : verified_devices) {
        const auto& port =
            device.port;

        headmotion::session::SavedDevicePort saved;

        saved.preferred_path =
            port.preferredPath();

        saved.system_path =
            port.path;

        saved.vendor_id =
            port.vendor_id;

        saved.product_id =
            port.product_id;

        saved.serial_number =
            port.serial_number;

        const auto store_path =
            headmotion::session::DevicePortStore::
                pathForDevice(
                    saved.serial_number
                );

        headmotion::session::DevicePortStore::save(
            store_path,
            saved
        );

        std::cout
            << "\n"
            << "  Device ID: "
            << saved.serial_number
            << "\n"
            << "    Port: "
            << saved.preferred_path
            << "\n"
            << "    System port: "
            << saved.system_path
            << "\n"
            << "    Identity: "
            << device.identity
            << "\n"
            << "    Port record: "
            << store_path
            << "\n";
    }

    std::cout
        << "\nScan complete. Registered "
        << verified_devices.size()
        << " MMS+ device";

    if (verified_devices.size() != 1) {
        std::cout << "s";
    }

    std::cout << ".\n";

    return 0;
}

} // namespace headmotion::app