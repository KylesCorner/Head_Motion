#include "headmotion/app/MmsDeviceProbe.hpp"
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

    /*
     * Determine a stable hardware identifier for display/runtime use.
     *
     * IMPORTANT:
     * This value is NOT persisted anywhere on the host.
     */
    std::string stableDeviceId(
        const headmotion::transport::SerialPortInfo& port
    ) {
        /*
         * Preferred source:
         * USB serial metadata supplied by the serial-port backend.
         *
         * This works on Windows when the backend exposes the USB
         * serial number.
         */
        if (!port.serial_number.empty()) {
            return port.serial_number;
        }

        /*
         * Linux fallback.
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
            constexpr const char* marker =
                "usb-MbientLab_MetaMotionS_";

            const std::size_t begin =
                port.symlink_path.find(marker);

            if (begin != std::string::npos) {
                const std::size_t serial_begin =
                    begin + std::string(marker).size();

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

        /*
         * A stable ID is useful, but it is no longer required for
         * host-side state lookup because the application is stateless.
         */
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
                    << std::setfill(' ')
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

            /*
             * USB VID/PID alone is not enough.
             *
             * Verify that the device actually responds to the
             * MetaMotionS protocol.
             */
            const auto probe =
                probeMmsDevice(
                    port.preferredPath()
                );

            if (!probe) {
                std::cout
                    << " - protocol verification failed\n";

                continue;
            }

            const std::string device_id =
                stableDeviceId(port);

            std::cout
                << " - verified: "
                << probe->identity;

            if (!device_id.empty()) {
                std::cout
                    << " device_id="
                    << device_id;
            }

            std::cout
                << "\n";

            verified_devices.push_back({
                .port = port,
                .identity = probe->identity,
                .device_id = device_id
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

        for (const auto& device : verified_devices) {
            const auto& port =
                device.port;

            std::cout
                << "\n";

            if (!device.device_id.empty()) {
                std::cout
                    << "  Device ID: "
                    << device.device_id
                    << "\n";
            }
            else {
                std::cout
                    << "  Device ID: unavailable\n";
            }

            std::cout
                << "  Port: "
                << port.preferredPath()
                << "\n"
                << "  System port: "
                << port.path
                << "\n"
                << "  Identity: "
                << device.identity
                << "\n";

            if (
                port.vendor_id != 0 ||
                port.product_id != 0
                ) {
                std::cout
                    << "  USB VID:PID: "
                    << std::hex
                    << std::setw(4)
                    << std::setfill('0')
                    << port.vendor_id
                    << ":"
                    << std::setw(4)
                    << port.product_id
                    << std::dec
                    << std::setfill(' ')
                    << "\n";
            }
        }

        std::cout
            << "\nScan complete. Found "
            << verified_devices.size()
            << " MMS+ device";

        if (verified_devices.size() != 1) {
            std::cout << "s";
        }

        std::cout
            << ".\n";

        return 0;
    }

} // namespace headmotion::app