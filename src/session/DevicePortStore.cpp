#include "headmotion/session/DevicePortStore.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace headmotion::session {

namespace {

namespace fs = std::filesystem;

constexpr std::array<char, 8> FILE_MAGIC = {
    'M', 'M', 'S', 'P', 'O', 'R', 'T', '1'
};

constexpr std::uint16_t FILE_VERSION = 1;
constexpr std::uint32_t MAX_STRING_LENGTH = 4096;

void writeBytes(
    std::ostream& out,
    const void* data,
    std::size_t size,
    const std::string& path
) {
    out.write(
        static_cast<const char*>(data),
        static_cast<std::streamsize>(size)
    );

    if (!out) {
        throw std::runtime_error(
            "Failed to write device port file: " + path
        );
    }
}

void writeUint16(
    std::ostream& out,
    std::uint16_t value,
    const std::string& path
) {
    const std::array<std::uint8_t, 2> bytes = {
        static_cast<std::uint8_t>(value & 0xffu),
        static_cast<std::uint8_t>((value >> 8u) & 0xffu)
    };

    writeBytes(out, bytes.data(), bytes.size(), path);
}

void writeUint32(
    std::ostream& out,
    std::uint32_t value,
    const std::string& path
) {
    const std::array<std::uint8_t, 4> bytes = {
        static_cast<std::uint8_t>(value & 0xffu),
        static_cast<std::uint8_t>((value >> 8u) & 0xffu),
        static_cast<std::uint8_t>((value >> 16u) & 0xffu),
        static_cast<std::uint8_t>((value >> 24u) & 0xffu)
    };

    writeBytes(out, bytes.data(), bytes.size(), path);
}

void writeString(
    std::ostream& out,
    const std::string& value,
    const std::string& path
) {
    if (value.size() > MAX_STRING_LENGTH) {
        throw std::runtime_error(
            "Device port field is too long"
        );
    }

    writeUint32(
        out,
        static_cast<std::uint32_t>(value.size()),
        path
    );

    if (!value.empty()) {
        writeBytes(
            out,
            value.data(),
            value.size(),
            path
        );
    }
}

void readBytes(
    std::istream& in,
    void* data,
    std::size_t size,
    const std::string& path
) {
    in.read(
        static_cast<char*>(data),
        static_cast<std::streamsize>(size)
    );

    if (!in) {
        throw std::runtime_error(
            "Device port file is truncated or unreadable: " + path
        );
    }
}

std::uint16_t readUint16(
    std::istream& in,
    const std::string& path
) {
    std::array<std::uint8_t, 2> bytes{};
    readBytes(in, bytes.data(), bytes.size(), path);

    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8u)
    );
}

std::uint32_t readUint32(
    std::istream& in,
    const std::string& path
) {
    std::array<std::uint8_t, 4> bytes{};
    readBytes(in, bytes.data(), bytes.size(), path);

    return
        static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

std::string readString(
    std::istream& in,
    const std::string& path
) {
    const std::uint32_t length =
        readUint32(in, path);

    if (length > MAX_STRING_LENGTH) {
        throw std::runtime_error(
            "Device port file contains an invalid string length: " +
            path
        );
    }

    std::string value(length, '\0');

    if (length != 0) {
        readBytes(
            in,
            value.data(),
            value.size(),
            path
        );
    }

    return value;
}

} // namespace

std::string DevicePortStore::defaultPath() {
    return "data/latest_device_port.bin";
}

void DevicePortStore::save(
    const std::string& path,
    const SavedDevicePort& port
) {
    if (port.preferred_path.empty() &&
        port.system_path.empty()) {
        throw std::runtime_error(
            "Refusing to save a device record with no port path"
        );
    }

    const fs::path destination{path};

    if (destination.has_parent_path()) {
        std::error_code error;

        fs::create_directories(
            destination.parent_path(),
            error
        );

        if (error) {
            throw std::runtime_error(
                "Failed to create device port directory: " +
                destination.parent_path().string() +
                ": " +
                error.message()
            );
        }
    }

    fs::path temporary = destination;
    temporary += ".tmp";

    {
        std::ofstream out(
            temporary,
            std::ios::binary |
            std::ios::out |
            std::ios::trunc
        );

        if (!out) {
            throw std::runtime_error(
                "Failed to open temporary device port file: " +
                temporary.string()
            );
        }

        writeBytes(
            out,
            FILE_MAGIC.data(),
            FILE_MAGIC.size(),
            temporary.string()
        );

        writeUint16(
            out,
            FILE_VERSION,
            temporary.string()
        );

        writeUint16(
            out,
            port.vendor_id,
            temporary.string()
        );

        writeUint16(
            out,
            port.product_id,
            temporary.string()
        );

        writeString(
            out,
            port.preferred_path,
            temporary.string()
        );

        writeString(
            out,
            port.system_path,
            temporary.string()
        );

        writeString(
            out,
            port.serial_number,
            temporary.string()
        );

        out.flush();

        if (!out) {
            throw std::runtime_error(
                "Failed to flush temporary device port file: " +
                temporary.string()
            );
        }
    }

    std::error_code rename_error;

    fs::rename(
        temporary,
        destination,
        rename_error
    );

    if (!rename_error) {
        return;
    }

    /*
     * Windows does not normally allow rename() to replace an existing file.
     * Remove the old record only after the complete temporary file exists.
     */
    std::error_code remove_error;
    fs::remove(destination, remove_error);

    if (remove_error && fs::exists(destination)) {
        fs::remove(temporary);

        throw std::runtime_error(
            "Failed to replace device port file: " +
            destination.string() +
            ": " +
            remove_error.message()
        );
    }

    rename_error.clear();

    fs::rename(
        temporary,
        destination,
        rename_error
    );

    if (rename_error) {
        fs::remove(temporary);

        throw std::runtime_error(
            "Failed to move temporary device port file into place: " +
            destination.string() +
            ": " +
            rename_error.message()
        );
    }
}

std::optional<SavedDevicePort> DevicePortStore::load(
    const std::string& path
) {
    std::error_code exists_error;
    const bool exists =
        fs::exists(path, exists_error);

    if (exists_error) {
        throw std::runtime_error(
            "Failed to inspect device port file: " +
            path +
            ": " +
            exists_error.message()
        );
    }

    if (!exists) {
        return std::nullopt;
    }

    std::ifstream in(
        path,
        std::ios::binary |
        std::ios::in
    );

    if (!in) {
        throw std::runtime_error(
            "Failed to open device port file: " + path
        );
    }

    std::array<char, FILE_MAGIC.size()> magic{};

    readBytes(
        in,
        magic.data(),
        magic.size(),
        path
    );

    if (magic != FILE_MAGIC) {
        throw std::runtime_error(
            "Invalid device port file format: " + path
        );
    }

    const std::uint16_t version =
        readUint16(in, path);

    if (version != FILE_VERSION) {
        throw std::runtime_error(
            "Unsupported device port file version: " +
            std::to_string(version)
        );
    }

    SavedDevicePort port;

    port.vendor_id =
        readUint16(in, path);

    port.product_id =
        readUint16(in, path);

    port.preferred_path =
        readString(in, path);

    port.system_path =
        readString(in, path);

    port.serial_number =
        readString(in, path);

    if (port.preferred_path.empty() &&
        port.system_path.empty()) {
        throw std::runtime_error(
            "Device port file contains no usable path: " + path
        );
    }

    if (in.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error(
            "Device port file contains unexpected trailing data: " +
            path
        );
    }

    return port;
}

std::filesystem::path DevicePortStore::configRoot() {
#ifdef _WIN32
    const char* home =
        std::getenv("USERPROFILE");
#else
    const char* home =
        std::getenv("HOME");
#endif

    if (home == nullptr || *home == '\0') {
        throw std::runtime_error(
            "Unable to determine user home directory"
        );
    }

    return std::filesystem::path(home)
        / ".config"
        / "headmotion";
}

std::filesystem::path DevicePortStore::pathForDevice(
    const std::string& device_id
) {
    if (device_id.empty()) {
        throw std::runtime_error(
            "Cannot create device port path without a device ID"
        );
    }

    return configRoot()
        / "devices"
        / device_id
        / "port.bin";
}

} // namespace headmotion::session