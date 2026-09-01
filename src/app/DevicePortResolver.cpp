#include "headmotion/app/DevicePortResolver.hpp"

#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/transport/SerialPortInfo.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
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

} // namespace

std::string resolveDevicePort(
    const std::optional<std::string>& explicit_port
) {
    /*
     * An explicit command-line port always wins.
     *
     * Examples:
     *
     *   mmsctl record-start --port /dev/ttyACM0
     *   mmsctl record-stop  --port /dev/ttyACM1
     *   mmsctl sync         --port COM4
     *
     * This is the primary mechanism for selecting one sensor when
     * multiple MMS+ devices are connected.
     */
    if (explicit_port.has_value()) {
        if (explicit_port->empty()) {
            throw std::runtime_error(
                "The explicit serial-port argument is empty"
            );
        }

        return *explicit_port;
    }

    /*
     * No explicit device was supplied.
     *
     * Automatically resolve only when exactly one MMS+ is currently
     * connected. Never guess between multiple physical sensors.
     */
    const std::vector<SerialPortInfo> ports =
        headmotion::transport::SerialPortFactory::
            listPorts();

    if (ports.empty()) {
        throw std::runtime_error(
            "No serial ports are currently available. "
            "Connect an MMS+ device or provide --port explicitly."
        );
    }

    std::vector<const SerialPortInfo*> mms_ports;

    for (const auto& port : ports) {
        if (!isMmsCandidate(port)) {
            continue;
        }

        mms_ports.push_back(
            &port
        );
    }

    if (mms_ports.empty()) {
        throw std::runtime_error(
            "No MMS+ serial devices are currently available. "
            "Connect an MMS+ device and run `mmsctl scan`."
        );
    }

    if (mms_ports.size() > 1) {
        std::string message =
            "Multiple MMS+ devices are currently connected. "
            "Specify the sensor with --port explicitly:";

        for (const auto* port : mms_ports) {
            message += "\n  ";
            message += preferredPath(*port);

            if (!port->serial_number.empty()) {
                message += " serial=";
                message += port->serial_number;
            }
        }

        throw std::runtime_error(
            message
        );
    }

    const std::string selected =
        preferredPath(
            *mms_ports.front()
        );

    if (selected.empty()) {
        throw std::runtime_error(
            "The detected MMS+ device has no usable serial-port path"
        );
    }

    return selected;
}

} // namespace headmotion::app