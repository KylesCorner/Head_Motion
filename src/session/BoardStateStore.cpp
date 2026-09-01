#include "headmotion/session/BoardStateStore.hpp"

#include "headmotion/transport/SerialPortFactory.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace headmotion::session {

namespace fs = std::filesystem;

std::filesystem::path BoardStateStore::configRoot() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif

    if (home == nullptr || *home == '\0') {
        throw std::runtime_error(
            "Unable to determine user home directory"
        );
    }

    return fs::path(home)
        / ".config"
        / "headmotion";
}

std::string BoardStateStore::deviceIdForPort(
    const std::string& port_name
) {
    const auto ports =
        headmotion::transport::SerialPortFactory::
            listPorts();

    for (const auto& port : ports) {
        const bool matches =
            port.path == port_name ||
            port.symlink_path == port_name ||
            port.preferredPath() == port_name;

        if (!matches) {
            continue;
        }

        /*
         * First choice: backend-provided USB serial.
         */
        if (!port.serial_number.empty()) {
            return port.serial_number;
        }

        /*
         * Linux fallback: extract the stable serial from /dev/serial/by-id.
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

        throw std::runtime_error(
            "Could not determine stable hardware ID for MMS+ on port: " +
            port_name
        );
    }

    throw std::runtime_error(
        "Could not find serial-port information for: " +
        port_name
    );
}

// std::string BoardStateStore::deviceIdForPort(
//     const std::string& port_name
// ) {
//     const auto ports =
//         headmotion::transport::SerialPortFactory::listPorts();

//     for (const auto& port : ports) {
//         const bool matches =
//             port.path == port_name ||
//             port.symlink_path == port_name ||
//             port.preferredPath() == port_name;

//         if (!matches) {
//             continue;
//         }

//         if (port.serial_number.empty()) {
//             throw std::runtime_error(
//                 "MMS+ device has no USB serial number: " +
//                 port_name
//             );
//         }

//         return port.serial_number;
//     }

//     throw std::runtime_error(
//         "Could not identify MMS+ hardware serial for port: " +
//         port_name
//     );
// }

std::filesystem::path BoardStateStore::deviceDirectory(
    const std::string& device_id
) {
    if (device_id.empty()) {
        throw std::runtime_error(
            "Cannot create device state path without a device ID"
        );
    }

    return configRoot()
        / "devices"
        / device_id;
}

std::filesystem::path BoardStateStore::boardStatePath(
    const std::string& device_id
) {
    return deviceDirectory(device_id)
        / "board_state.bin";
}

std::filesystem::path BoardStateStore::loggerMetadataPath(
    const std::string& device_id
) {
    return deviceDirectory(device_id)
        / "logger_metadata.txt";
}

void BoardStateStore::save(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes
) {
    if (bytes.empty()) {
        throw std::runtime_error(
            "Refusing to save empty board state"
        );
    }

    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path()
        );
    }

    std::ofstream out(
        path,
        std::ios::binary |
        std::ios::out |
        std::ios::trunc
    );

    if (!out) {
        throw std::runtime_error(
            "Failed to open board state file for write: " +
            path.string()
        );
    }

    out.write(
        reinterpret_cast<const char*>(
            bytes.data()
        ),
        static_cast<std::streamsize>(
            bytes.size()
        )
    );

    if (!out) {
        throw std::runtime_error(
            "Failed to write board state file: " +
            path.string()
        );
    }
}

std::vector<std::uint8_t> BoardStateStore::load(
    const std::filesystem::path& path
) {
    std::ifstream in(
        path,
        std::ios::binary |
        std::ios::in
    );

    if (!in) {
        throw std::runtime_error(
            "Failed to open board state file for read: " +
            path.string()
        );
    }

    in.seekg(
        0,
        std::ios::end
    );

    const auto size =
        in.tellg();

    in.seekg(
        0,
        std::ios::beg
    );

    if (size <= 0) {
        throw std::runtime_error(
            "Board state file is empty: " +
            path.string()
        );
    }

    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(size)
    );

    in.read(
        reinterpret_cast<char*>(
            bytes.data()
        ),
        static_cast<std::streamsize>(
            bytes.size()
        )
    );

    if (!in) {
        throw std::runtime_error(
            "Failed to read board state file: " +
            path.string()
        );
    }

    return bytes;
}

} // namespace headmotion::session