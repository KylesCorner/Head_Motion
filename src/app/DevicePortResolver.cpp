#include "headmotion/app/DevicePortResolver.hpp"

#include "headmotion/session/DevicePortStore.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/transport/SerialPortInfo.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace headmotion::app {

namespace {

using headmotion::session::SavedDevicePort;
using headmotion::transport::SerialPortInfo;

std::string preferredPath(const SerialPortInfo& port) {
    if (!port.symlink_path.empty()) {
        return port.symlink_path;
    }

    return port.path;
}

bool usbIdsMatch(
    const SavedDevicePort& saved,
    const SerialPortInfo& current
) {
    /*
     * A zero value means the metadata was unavailable when the record
     * was saved or when the current port was discovered.
     *
     * Only reject the match when both sides have a value and those
     * values disagree.
     */
    if (saved.vendor_id != 0 &&
        current.vendor_id != 0 &&
        saved.vendor_id != current.vendor_id) {
        return false;
    }

    if (saved.product_id != 0 &&
        current.product_id != 0 &&
        saved.product_id != current.product_id) {
        return false;
    }

    return true;
}

bool pathMatches(
    const SavedDevicePort& saved,
    const SerialPortInfo& current
) {
    const std::string current_preferred =
        preferredPath(current);

    if (!saved.preferred_path.empty()) {
        if (current_preferred == saved.preferred_path ||
            current.path == saved.preferred_path ||
            current.symlink_path == saved.preferred_path) {
            return true;
        }
    }

    if (!saved.system_path.empty()) {
        if (current_preferred == saved.system_path ||
            current.path == saved.system_path ||
            current.symlink_path == saved.system_path) {
            return true;
        }
    }

    return false;
}

std::vector<const SerialPortInfo*> findPathMatches(
    const SavedDevicePort& saved,
    const std::vector<SerialPortInfo>& ports
) {
    std::vector<const SerialPortInfo*> matches;

    for (const auto& port : ports) {
        if (!pathMatches(saved, port)) {
            continue;
        }

        /*
         * Prevent a reused /dev/ttyACM* or COM number from silently
         * selecting different hardware.
         */
        if (!usbIdsMatch(saved, port)) {
            continue;
        }

        matches.push_back(&port);
    }

    return matches;
}

std::vector<const SerialPortInfo*> findSerialMatches(
    const SavedDevicePort& saved,
    const std::vector<SerialPortInfo>& ports
) {
    std::vector<const SerialPortInfo*> matches;

    if (saved.serial_number.empty()) {
        return matches;
    }

    for (const auto& port : ports) {
        if (port.serial_number.empty()) {
            continue;
        }

        if (port.serial_number != saved.serial_number) {
            continue;
        }

        if (!usbIdsMatch(saved, port)) {
            continue;
        }

        matches.push_back(&port);
    }

    return matches;
}

std::string describeSavedDevice(
    const SavedDevicePort& saved
) {
    std::string description;

    if (!saved.serial_number.empty()) {
        description += " serial=";
        description += saved.serial_number;
    }

    if (!saved.preferred_path.empty()) {
        description += " saved-path=";
        description += saved.preferred_path;
    } else if (!saved.system_path.empty()) {
        description += " saved-path=";
        description += saved.system_path;
    }

    return description;
}

std::string requireSingleMatch(
    const std::vector<const SerialPortInfo*>& matches,
    const std::string& match_type,
    const SavedDevicePort& saved
) {
    if (matches.empty()) {
        return {};
    }

    if (matches.size() > 1) {
        throw std::runtime_error(
            "Multiple serial ports matched the saved MMS+ " +
            match_type +
            " record." +
            describeSavedDevice(saved) +
            ". Run `mmsctl scan` again or provide `--port` explicitly."
        );
    }

    const std::string selected =
        preferredPath(*matches.front());

    if (selected.empty()) {
        throw std::runtime_error(
            "The matched MMS+ serial-port record contains no usable path"
        );
    }

    return selected;
}

} // namespace

std::string resolveDevicePort(
    const std::optional<std::string>& explicit_port
) {
    /*
     * An explicit command-line port always wins.
     *
     * This allows:
     *   mmsctl record-start --port /dev/ttyACM1
     *   mmsctl record-start COM4
     *
     * It also provides a recovery path if the saved device record is
     * stale or damaged.
     */
    if (explicit_port.has_value()) {
        if (explicit_port->empty()) {
            throw std::runtime_error(
                "The explicit serial-port argument is empty"
            );
        }

        return *explicit_port;
    }

    const std::string store_path =
        headmotion::session::DevicePortStore::defaultPath();

    const std::optional<SavedDevicePort> saved =
        headmotion::session::DevicePortStore::load(
            store_path
        );

    if (!saved.has_value()) {
        throw std::runtime_error(
            "No saved MMS+ serial port was found. "
            "Run `mmsctl scan` to discover and save the device, "
            "or provide `--port` explicitly."
        );
    }

    const std::vector<SerialPortInfo> ports =
        headmotion::transport::SerialPortFactory::listPorts();

    if (ports.empty()) {
        throw std::runtime_error(
            "No serial ports are currently available. "
            "Reconnect the MMS+ device and run `mmsctl scan`, "
            "or provide `--port` explicitly."
        );
    }

    /*
     * First try the saved path.
     *
     * On Linux this will normally select the stable
     * /dev/serial/by-id/... path.
     *
     * On Windows this will eventually select the saved COM port.
     */
    const std::vector<const SerialPortInfo*> path_matches =
        findPathMatches(*saved, ports);

    const std::string path_result =
        requireSingleMatch(
            path_matches,
            "path",
            *saved
        );

    if (!path_result.empty()) {
        return path_result;
    }

    /*
     * The OS path may change:
     *
     *   /dev/ttyACM0 -> /dev/ttyACM1
     *   COM3         -> COM5
     *
     * Recover using the stable hardware serial number and VID/PID.
     */
    const std::vector<const SerialPortInfo*> serial_matches =
        findSerialMatches(*saved, ports);

    const std::string serial_result =
        requireSingleMatch(
            serial_matches,
            "serial-number",
            *saved
        );

    if (!serial_result.empty()) {
        return serial_result;
    }

    throw std::runtime_error(
        "The saved MMS+ device is not currently available." +
        describeSavedDevice(*saved) +
        ". Reconnect it and run `mmsctl scan`, "
        "or provide `--port` explicitly."
    );
}

} // namespace headmotion::app