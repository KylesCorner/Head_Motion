#include "headmotion/app/DevicePortResolver.hpp"

#include "headmotion/app/MmsDeviceProbe.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/transport/SerialPortInfo.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace headmotion::app {

    namespace {

        using headmotion::transport::SerialPortInfo;

        constexpr std::uint16_t MMS_VENDOR_ID = 0x1915;
        constexpr std::uint16_t MMS_PRODUCT_ID = 0xd978;

        std::string preferredPath(
            const SerialPortInfo& port
        ) {
            if (!port.symlink_path.empty()) {
                return port.symlink_path;
            }

            return port.path;
        }

        bool exactMmsUsbMatch(
            const SerialPortInfo& port
        ) {
            return
                port.vendor_id == MMS_VENDOR_ID &&
                port.product_id == MMS_PRODUCT_ID;
        }

        bool isMmsCandidate(
            const SerialPortInfo& port
        ) {
            return
                exactMmsUsbMatch(port) ||
                port.likely_mms;
        }

        /*
         * First try platform USB metadata / Linux by-id.
         * The MMS identity response is handled separately below because probing
         * requires opening the serial port.
         */
        std::string metadataDeviceId(
            const SerialPortInfo& port
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

            return {};
        }

        std::string probedDeviceId(
            const SerialPortInfo& port
        ) {
            const std::string path =
                preferredPath(port);

            if (path.empty()) {
                return {};
            }

            const auto probe =
                probeMmsDevice(path);

            if (!probe.has_value()) {
                return {};
            }

            return deviceIdFromMmsIdentity(
                probe->identity
            );
        }

        std::string bestDeviceId(
            const SerialPortInfo& port
        ) {
            /*
             * Prefer the board serial reported by the MMS itself.
             * Example identity:
             *
             *   MbientLab MetaMotionS 8 0.1 1.7.2 0561F6
             *
             * This is exactly the ID the user sees from identify/scan.
             */
            const std::string board_id =
                probedDeviceId(port);

            if (!board_id.empty()) {
                return board_id;
            }

            return metadataDeviceId(port);
        }

    } // namespace

    DevicePortError::DevicePortError(
        DevicePortErrorReason reason,
        const std::string& message
    )
        : std::runtime_error(message),
        reason_(reason) {
    }

    DevicePortErrorReason DevicePortError::reason() const noexcept {
        return reason_;
    }

    std::string resolveDevicePort(
        const std::optional<std::string>& explicit_port,
        const std::optional<std::string>& device_id
    ) {
        if (explicit_port.has_value() && device_id.has_value()) {
            throw DevicePortError(
                DevicePortErrorReason::selector_conflict,
                "Specify either --port or --device-id, not both"
            );
        }

        if (explicit_port.has_value()) {
            if (explicit_port->empty()) {
                throw DevicePortError(
                    DevicePortErrorReason::invalid_selector,
                    "The explicit serial-port argument is empty"
                );
            }

            return *explicit_port;
        }

        if (device_id.has_value() && device_id->empty()) {
            throw DevicePortError(
                DevicePortErrorReason::invalid_selector,
                "--device-id requires a non-empty value"
            );
        }

        const std::vector<SerialPortInfo> ports =
            headmotion::transport::SerialPortFactory::
            listPorts();

        if (ports.empty()) {
            throw DevicePortError(
                DevicePortErrorReason::no_serial_ports,
                "No serial ports are currently available. Connect an MMS+ device."
            );
        }

        std::vector<const SerialPortInfo*> mms_ports;

        for (const auto& port : ports) {
            if (!isMmsCandidate(port)) {
                continue;
            }

            mms_ports.push_back(&port);
        }

        if (mms_ports.empty()) {
            throw DevicePortError(
                DevicePortErrorReason::no_mms_devices,
                "No MMS+ serial devices are currently available"
            );
        }

        if (device_id.has_value()) {
            for (const auto* port : mms_ports) {
                /*
                 * Prefer the board-reported serial but retain compatibility with
                 * USB serial metadata / Linux by-id values.
                 */
                const std::string board_id =
                    probedDeviceId(*port);

                const std::string metadata_id =
                    metadataDeviceId(*port);

                if (
                    board_id != *device_id &&
                    metadata_id != *device_id
                    ) {
                    continue;
                }

                const std::string selected =
                    preferredPath(*port);

                if (selected.empty()) {
                    throw DevicePortError(
                        DevicePortErrorReason::invalid_selector,
                        "The selected MMS+ device has no usable serial-port path"
                    );
                }

                return selected;
            }

            throw DevicePortError(
                DevicePortErrorReason::device_id_not_found,
                "No connected MMS+ device matches --device-id " +
                *device_id
            );
        }

        if (mms_ports.size() > 1) {
            std::string message =
                "Multiple MMS+ devices are currently connected. "
                "Specify one with --port or --device-id:";

            for (const auto* port : mms_ports) {
                message += "\n  ";
                message += preferredPath(*port);

                const std::string id =
                    bestDeviceId(*port);

                if (!id.empty()) {
                    message += " device_id=";
                    message += id;
                }
            }

            throw DevicePortError(
                DevicePortErrorReason::multiple_mms_devices,
                message
            );
        }

        const std::string selected =
            preferredPath(*mms_ports.front());

        if (selected.empty()) {
            throw DevicePortError(
                DevicePortErrorReason::invalid_selector,
                "The detected MMS+ device has no usable serial-port path"
            );
        }

        return selected;
    }

} // namespace headmotion::app
