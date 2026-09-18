#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/app/MmsDeviceProbe.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <cstdint>
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
        if (!port.serial_number.empty()) {
            return port.serial_number;
        }

        if (!port.symlink_path.empty()) {
            constexpr const char* marker =
                "usb-MbientLab_MetaMotionS_";

            const std::size_t begin =
                port.symlink_path.find(marker);

            if (begin != std::string::npos) {
                const std::size_t serial_begin =
                    begin +
                    std::string(marker).size();

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

    int runScanPortsCommand(
        CommandOutput& output
    ) {
        output.started();

        const auto ports =
            headmotion::transport::SerialPortFactory::
            listPorts();

        if (ports.empty()) {
            output.error(
                "no_serial_ports",
                "No serial ports found"
            );

            output.completed(
                false,
                2
            );

            return 2;
        }

        output.event(
            "scan_summary",
            {
                {
                    "serial_ports_found",
                    static_cast<std::uint64_t>(
                        ports.size()
                    )
                }
            }
        );

        std::vector<VerifiedMms> verified_devices;

        for (const auto& port : ports) {
            const bool candidate =
                exactMmsUsbMatch(port) ||
                port.likely_mms;

            output.event(
                "port",
                {
                    {"path", port.path},
                    {
                        "preferred_path",
                        port.preferredPath()
                    },
                    {
                        "symlink_path",
                        port.symlink_path
                    },
                    {
                        "serial_number",
                        port.serial_number
                    },
                    {
                        "vendor_id",
                        port.vendor_id
                    },
                    {
                        "product_id",
                        port.product_id
                    },
                    {
                        "candidate",
                        candidate
                    }
                }
            );

            if (!candidate) {
                continue;
            }

            const auto probe =
                probeMmsDevice(
                    port.preferredPath()
                );

            if (!probe) {
                output.warning(
                    "protocol_verification_failed",
                    "MMS+ candidate did not respond to "
                    "the MetaMotionS protocol on " +
                    port.preferredPath()
                );

                continue;
            }

            const std::string device_id =
                stableDeviceId(port);

            output.event(
                "device",
                {
                    {
                        "device_id",
                        device_id
                    },
                    {
                        "port",
                        port.preferredPath()
                    },
                    {
                        "system_port",
                        port.path
                    },
                    {
                        "identity",
                        probe->identity
                    },
                    {
                        "vendor_id",
                        port.vendor_id
                    },
                    {
                        "product_id",
                        port.product_id
                    }
                }
            );

            verified_devices.push_back(
                {
                    .port = port,
                    .identity = probe->identity,
                    .device_id = device_id
                }
            );
        }

        if (verified_devices.empty()) {
            output.error(
                "no_verified_devices",
                "No verified MMS+ devices found"
            );

            output.completed(
                false,
                3
            );

            return 3;
        }

        output.event(
            "summary",
            {
                {
                    "verified_devices",
                    static_cast<std::uint64_t>(
                        verified_devices.size()
                    )
                }
            }
        );

        output.completed(
            true,
            0
        );

        return 0;
    }

    int runScanPortsCommand() {
        CommandOutput output("scan");

        return runScanPortsCommand(
            output
        );
    }

} // namespace headmotion::app
