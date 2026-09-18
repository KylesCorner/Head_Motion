#include "WindowsSerialDiscovery.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <devguid.h>
#include <setupapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace headmotion::platform::windows_platform {

namespace {

class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO handle)
        : handle_(handle) {}

    ~DeviceInfoSet() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    HDEVINFO get() const {
        return handle_;
    }

private:
    HDEVINFO handle_ = INVALID_HANDLE_VALUE;
};

std::string windowsErrorMessage(
    const std::string& operation,
    DWORD error
) {
    return operation + " failed with Windows error " +
           std::to_string(error);
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (required <= 0) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "WideCharToMultiByte"
        );
    }

    std::string result(
        static_cast<std::size_t>(required),
        '\0'
    );

    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr
    );

    if (written != required) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "WideCharToMultiByte"
        );
    }

    return result;
}

std::string lowerCopy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value;
}

std::string upperCopy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        }
    );

    return value;
}

std::optional<std::wstring> getDevicePropertyString(
    HDEVINFO device_info_set,
    SP_DEVINFO_DATA& device_info,
    DWORD property
) {
    DWORD property_type = 0;
    DWORD required_size = 0;

    SetupDiGetDeviceRegistryPropertyW(
        device_info_set,
        &device_info,
        property,
        &property_type,
        nullptr,
        0,
        &required_size
    );

    const DWORD first_error = GetLastError();

    if (required_size == 0) {
        if (first_error == ERROR_INVALID_DATA ||
            first_error == ERROR_NOT_FOUND) {
            return std::nullopt;
        }

        return std::nullopt;
    }

    std::vector<BYTE> buffer(required_size);

    if (!SetupDiGetDeviceRegistryPropertyW(
            device_info_set,
            &device_info,
            property,
            &property_type,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr
        )) {
        return std::nullopt;
    }

    if (property_type != REG_SZ &&
        property_type != REG_EXPAND_SZ &&
        property_type != REG_MULTI_SZ) {
        return std::nullopt;
    }

    const auto* text =
        reinterpret_cast<const wchar_t*>(buffer.data());

    if (text == nullptr || *text == L'\0') {
        return std::nullopt;
    }

    // For REG_MULTI_SZ (for example SPDRP_HARDWAREID), using the first
    // string is sufficient for VID/PID matching.
    return std::wstring(text);
}

std::optional<std::wstring> getPortName(
    HDEVINFO device_info_set,
    SP_DEVINFO_DATA& device_info
) {
    HKEY key = SetupDiOpenDevRegKey(
        device_info_set,
        &device_info,
        DICS_FLAG_GLOBAL,
        0,
        DIREG_DEV,
        KEY_READ
    );

    if (key == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD bytes = 0;

    LONG result = RegQueryValueExW(
        key,
        L"PortName",
        nullptr,
        &type,
        nullptr,
        &bytes
    );

    if (result != ERROR_SUCCESS ||
        bytes == 0 ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(
        (bytes / sizeof(wchar_t)) + 1,
        L'\0'
    );

    result = RegQueryValueExW(
        key,
        L"PortName",
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(buffer.data()),
        &bytes
    );

    RegCloseKey(key);

    if (result != ERROR_SUCCESS) {
        return std::nullopt;
    }

    std::wstring port_name(buffer.data());

    if (port_name.empty()) {
        return std::nullopt;
    }

    return port_name;
}

std::optional<std::wstring> getDeviceInstanceId(
    HDEVINFO device_info_set,
    SP_DEVINFO_DATA& device_info
) {
    DWORD required_chars = 0;

    SetupDiGetDeviceInstanceIdW(
        device_info_set,
        &device_info,
        nullptr,
        0,
        &required_chars
    );

    if (required_chars == 0) {
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(required_chars),
        L'\0'
    );

    if (!SetupDiGetDeviceInstanceIdW(
            device_info_set,
            &device_info,
            buffer.data(),
            required_chars,
            nullptr
        )) {
        return std::nullopt;
    }

    return std::wstring(buffer.data());
}

std::uint16_t parseHex16(
    const std::string& text,
    const std::string& marker
) {
    const std::string upper = upperCopy(text);
    const std::string upper_marker = upperCopy(marker);

    const std::size_t pos =
        upper.find(upper_marker);

    if (pos == std::string::npos) {
        return 0;
    }

    const std::size_t begin =
        pos + upper_marker.size();

    if (begin + 4 > upper.size()) {
        return 0;
    }

    try {
        const unsigned long value =
            std::stoul(
                upper.substr(begin, 4),
                nullptr,
                16
            );

        if (value > 0xffffUL) {
            return 0;
        }

        return static_cast<std::uint16_t>(value);
    } catch (...) {
        return 0;
    }
}

std::string extractSerialNumber(
    const std::string& instance_id
) {
    const std::size_t slash =
        instance_id.find_last_of("\\/");

    if (slash == std::string::npos ||
        slash + 1 >= instance_id.size()) {
        return {};
    }

    std::string serial =
        instance_id.substr(slash + 1);

    // USB devices with a real serial number generally expose that value as
    // the final instance-ID component. If Windows generated a location-style
    // identifier instead, it commonly contains '&'; do not persist that as a
    // hardware serial number.
    if (serial.find('&') != std::string::npos) {
        return {};
    }

    return serial;
}

bool isComPortName(const std::string& value) {
    if (value.size() < 4) {
        return false;
    }

    const std::string upper = upperCopy(value);

    if (upper.rfind("COM", 0) != 0) {
        return false;
    }

    return std::all_of(
        upper.begin() + 3,
        upper.end(),
        [](unsigned char c) {
            return std::isdigit(c) != 0;
        }
    );
}

void appendWineRegistryPorts(
    std::vector<headmotion::transport::SerialPortInfo>& ports
) {
    HKEY key = nullptr;

    const LONG open_result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"Software\\Wine\\Ports",
        0,
        KEY_READ,
        &key
    );

    if (open_result != ERROR_SUCCESS) {
        // Expected on normal Windows installations.
        return;
    }

    DWORD value_count = 0;
    DWORD max_value_name_chars = 0;
    DWORD max_value_data_bytes = 0;

    const LONG query_result = RegQueryInfoKeyW(
        key,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &value_count,
        &max_value_name_chars,
        &max_value_data_bytes,
        nullptr,
        nullptr
    );

    if (query_result != ERROR_SUCCESS) {
        RegCloseKey(key);
        return;
    }

    std::vector<wchar_t> value_name(
        static_cast<std::size_t>(max_value_name_chars) + 2,
        L'\0'
    );

    std::vector<BYTE> value_data(
        static_cast<std::size_t>(max_value_data_bytes) +
            sizeof(wchar_t),
        0
    );

    for (DWORD index = 0; index < value_count; ++index) {
        std::fill(
            value_name.begin(),
            value_name.end(),
            L'\0'
        );

        std::fill(
            value_data.begin(),
            value_data.end(),
            0
        );

        DWORD value_name_length =
            static_cast<DWORD>(
                value_name.size() - 1
            );

        DWORD value_data_size =
            static_cast<DWORD>(
                value_data.size() - sizeof(wchar_t)
            );

        DWORD value_type = 0;

        const LONG result = RegEnumValueW(
            key,
            index,
            value_name.data(),
            &value_name_length,
            nullptr,
            &value_type,
            value_data.data(),
            &value_data_size
        );

        if (result != ERROR_SUCCESS) {
            continue;
        }

        if (value_type != REG_SZ &&
            value_type != REG_EXPAND_SZ) {
            continue;
        }

        const std::string port_name =
            wideToUtf8(
                std::wstring(
                    value_name.data(),
                    value_name_length
                )
            );

        if (!isComPortName(port_name)) {
            continue;
        }

        const bool already_present =
            std::any_of(
                ports.begin(),
                ports.end(),
                [&](const auto& port) {
                    return upperCopy(port.path) ==
                           upperCopy(port_name);
                }
            );

        if (already_present) {
            continue;
        }

        const auto* mapped_path_w =
            reinterpret_cast<const wchar_t*>(
                value_data.data()
            );

        const std::string mapped_path =
            wideToUtf8(
                std::wstring(mapped_path_w)
            );

        headmotion::transport::SerialPortInfo info;

        info.path = upperCopy(port_name);
        info.display_name =
            "Wine serial port " + info.path;

        info.manufacturer = "Wine";
        info.product_name = mapped_path;

        /*
         * Wine does not reliably expose manually mapped serial ports through
         * SetupAPI with USB VID/PID metadata. Treat explicitly registered Wine
         * ports as candidates and let probeMmsDevice() perform the actual MMS+
         * protocol verification.
         */
        info.likely_mms = true;

        ports.push_back(
            std::move(info)
        );
    }

    RegCloseKey(key);
}

} // namespace

std::vector<headmotion::transport::SerialPortInfo>
WindowsSerialDiscovery::listPorts() const {
    std::vector<headmotion::transport::SerialPortInfo> ports;

    const HDEVINFO raw_device_info_set =
        SetupDiGetClassDevsW(
            &GUID_DEVCLASS_PORTS,
            nullptr,
            nullptr,
            DIGCF_PRESENT
        );

    if (raw_device_info_set != INVALID_HANDLE_VALUE) {
        DeviceInfoSet device_info_set(
            raw_device_info_set
        );

        for (DWORD index = 0;; ++index) {
            SP_DEVINFO_DATA device_info{};
            device_info.cbSize = sizeof(device_info);

            if (!SetupDiEnumDeviceInfo(
                    device_info_set.get(),
                    index,
                    &device_info
                )) {
                const DWORD error = GetLastError();

                if (error == ERROR_NO_MORE_ITEMS) {
                    break;
                }

                throw std::runtime_error(
                    windowsErrorMessage(
                        "SetupDiEnumDeviceInfo",
                        error
                    )
                );
            }

            const auto port_name_w =
                getPortName(
                    device_info_set.get(),
                    device_info
                );

            if (!port_name_w) {
                continue;
            }

            const std::string port_name =
                wideToUtf8(*port_name_w);

            if (!isComPortName(port_name)) {
                continue;
            }

            headmotion::transport::SerialPortInfo info;

            info.path = port_name;
            info.symlink_path.clear();

            if (const auto friendly =
                    getDevicePropertyString(
                        device_info_set.get(),
                        device_info,
                        SPDRP_FRIENDLYNAME
                    )) {
                info.display_name =
                    wideToUtf8(*friendly);
            }

            if (info.display_name.empty()) {
                if (const auto description =
                        getDevicePropertyString(
                            device_info_set.get(),
                            device_info,
                            SPDRP_DEVICEDESC
                        )) {
                    info.display_name =
                        wideToUtf8(*description);
                }
            }

            if (info.display_name.empty()) {
                info.display_name = port_name;
            }

            if (const auto manufacturer =
                    getDevicePropertyString(
                        device_info_set.get(),
                        device_info,
                        SPDRP_MFG
                    )) {
                info.manufacturer =
                    wideToUtf8(*manufacturer);
            }

            if (const auto description =
                    getDevicePropertyString(
                        device_info_set.get(),
                        device_info,
                        SPDRP_DEVICEDESC
                    )) {
                info.product_name =
                    wideToUtf8(*description);
            }

            std::string hardware_id;

            if (const auto hardware_id_w =
                    getDevicePropertyString(
                        device_info_set.get(),
                        device_info,
                        SPDRP_HARDWAREID
                    )) {
                hardware_id =
                    wideToUtf8(*hardware_id_w);

                info.vendor_id =
                    parseHex16(
                        hardware_id,
                        "VID_"
                    );

                info.product_id =
                    parseHex16(
                        hardware_id,
                        "PID_"
                    );
            }

            std::string instance_id;

            if (const auto instance_id_w =
                    getDeviceInstanceId(
                        device_info_set.get(),
                        device_info
                    )) {
                instance_id =
                    wideToUtf8(*instance_id_w);

                info.serial_number =
                    extractSerialNumber(
                        instance_id
                    );

                if (info.vendor_id == 0) {
                    info.vendor_id =
                        parseHex16(
                            instance_id,
                            "VID_"
                        );
                }

                if (info.product_id == 0) {
                    info.product_id =
                        parseHex16(
                            instance_id,
                            "PID_"
                        );
                }
            }

            constexpr std::uint16_t MMS_VENDOR_ID =
                0x1915;

            constexpr std::uint16_t MMS_PRODUCT_ID =
                0xd978;

            const bool exact_usb_match =
                info.vendor_id == MMS_VENDOR_ID &&
                info.product_id == MMS_PRODUCT_ID;

            const std::string combined =
                info.path + " " +
                info.display_name + " " +
                info.manufacturer + " " +
                info.product_name + " " +
                hardware_id + " " +
                instance_id;

            info.likely_mms =
                exact_usb_match ||
                looksLikeMmsName(combined);

            ports.push_back(
                std::move(info)
            );
        }
    } else {
        const DWORD error = GetLastError();

        /*
         * Wine may not provide the Windows ports device class through SetupAPI
         * even though explicit Wine COM mappings exist. On native Windows,
         * preserve the existing behavior and report unexpected SetupAPI errors.
         */
        HKEY wine_ports_key = nullptr;

        const LONG wine_key_result = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"Software\\Wine\\Ports",
            0,
            KEY_READ,
            &wine_ports_key
        );

        if (wine_key_result == ERROR_SUCCESS) {
            RegCloseKey(wine_ports_key);
        } else {
            throw std::runtime_error(
                windowsErrorMessage(
                    "SetupDiGetClassDevsW",
                    error
                )
            );
        }
    }

    /*
     * Wine serial mappings may not appear through SetupAPI. Add explicitly
     * configured Wine COM ports as protocol-verification candidates.
     *
     * On native Windows the registry key normally does not exist, so this is
     * effectively a no-op.
     */
    appendWineRegistryPorts(ports);

    std::sort(
        ports.begin(),
        ports.end(),
        [](const auto& a, const auto& b) {
            if (a.likely_mms != b.likely_mms) {
                return a.likely_mms >
                       b.likely_mms;
            }

            return a.path < b.path;
        }
    );

    return ports;
}

bool WindowsSerialDiscovery::looksLikeMmsName(
    const std::string& value
) {
    const std::string lower =
        lowerCopy(value);

    return
        lower.find("mbientlab") !=
            std::string::npos ||
        lower.find("metamotions") !=
            std::string::npos ||
        lower.find("metawear") !=
            std::string::npos;
}

} // namespace headmotion::platform::windows_platform
