#include "WindowsSerialPort.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace headmotion::platform::windows_platform {

namespace {

[[noreturn]]
void throwWindowsError(const std::string& operation) {
    const DWORD error = GetLastError();

    throw std::system_error(
        static_cast<int>(error),
        std::system_category(),
        operation
    );
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );

    if (required <= 0) {
        throwWindowsError("MultiByteToWideChar");
    }

    std::wstring result(
        static_cast<std::size_t>(required),
        L'\0'
    );

    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required
    );

    if (written != required) {
        throwWindowsError("MultiByteToWideChar");
    }

    return result;
}

std::wstring normalizePortName(const std::string& port_name) {
    if (port_name.empty()) {
        throw std::runtime_error("Serial port name is empty");
    }

    constexpr const char* DEVICE_PREFIX = R"(\\.\)";

    if (port_name.rfind(DEVICE_PREFIX, 0) == 0) {
        return utf8ToWide(port_name);
    }

    return utf8ToWide(
        std::string(DEVICE_PREFIX) + port_name
    );
}

DWORD checkedTimeout(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        return 0;
    }

    constexpr auto MAX_DWORD =
        static_cast<unsigned long long>(
            std::numeric_limits<DWORD>::max()
        );

    const auto value =
        static_cast<unsigned long long>(
            timeout.count()
        );

    return static_cast<DWORD>(
        std::min(value, MAX_DWORD)
    );
}

} // namespace

WindowsSerialPort::WindowsSerialPort(
    headmotion::transport::SerialConfig config
)
    : config_(std::move(config)) {}

WindowsSerialPort::~WindowsSerialPort() {
    close();
}

WindowsSerialPort::WindowsSerialPort(
    WindowsSerialPort&& other
) noexcept
    : config_(std::move(other.config_)),
      handle_(
          std::exchange(
              other.handle_,
              INVALID_HANDLE_VALUE
          )
      ) {}

WindowsSerialPort&
WindowsSerialPort::operator=(
    WindowsSerialPort&& other
) noexcept {
    if (this != &other) {
        close();

        config_ = std::move(other.config_);

        handle_ = std::exchange(
            other.handle_,
            INVALID_HANDLE_VALUE
        );
    }

    return *this;
}

void WindowsSerialPort::open() {
    close();

    const std::wstring device_path =
        normalizePortName(config_.port_name);

    handle_ = CreateFileW(
        device_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (handle_ == INVALID_HANDLE_VALUE) {
        throwWindowsError(
            "CreateFile(" + config_.port_name + ")"
        );
    }

    try {
        if (!SetupComm(handle_, 4096, 4096)) {
            throwWindowsError("SetupComm");
        }

        configurePort();
        configureTimeouts(std::chrono::milliseconds{0});

        if (config_.open_delay.count() > 0) {
            std::this_thread::sleep_for(
                config_.open_delay
            );
        }

        if (!PurgeComm(
                handle_,
                PURGE_RXABORT |
                PURGE_RXCLEAR |
                PURGE_TXABORT |
                PURGE_TXCLEAR
            )) {
            throwWindowsError("PurgeComm");
        }
    } catch (...) {
        close();
        throw;
    }
}

void WindowsSerialPort::close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

bool WindowsSerialPort::isOpen() const {
    return handle_ != INVALID_HANDLE_VALUE;
}

void WindowsSerialPort::write(
    const std::vector<std::uint8_t>& bytes
) {
    ensureOpen();

    if (bytes.empty()) {
        return;
    }

    std::size_t offset = 0;

    while (offset < bytes.size()) {
        const std::size_t remaining =
            bytes.size() - offset;

        const DWORD chunk_size =
            static_cast<DWORD>(
                std::min<std::size_t>(
                    remaining,
                    std::numeric_limits<DWORD>::max()
                )
            );

        DWORD written = 0;

        const BOOL ok = WriteFile(
            handle_,
            bytes.data() + offset,
            chunk_size,
            &written,
            nullptr
        );

        if (!ok) {
            throwWindowsError("WriteFile");
        }

        if (written == 0) {
            throw std::runtime_error(
                "Serial write returned zero bytes"
            );
        }

        offset += static_cast<std::size_t>(written);
    }

    if (!FlushFileBuffers(handle_)) {
        throwWindowsError("FlushFileBuffers");
    }
}

std::vector<std::uint8_t>
WindowsSerialPort::read(
    std::size_t max_bytes,
    std::chrono::milliseconds timeout
) {
    ensureOpen();

    std::vector<std::uint8_t> out;

    if (max_bytes == 0) {
        return out;
    }

    configureTimeouts(timeout);

    const DWORD requested =
        static_cast<DWORD>(
            std::min<std::size_t>(
                max_bytes,
                std::numeric_limits<DWORD>::max()
            )
        );

    out.resize(
        static_cast<std::size_t>(requested)
    );

    DWORD bytes_read = 0;

    const BOOL ok = ReadFile(
        handle_,
        out.data(),
        requested,
        &bytes_read,
        nullptr
    );

    if (!ok) {
        throwWindowsError("ReadFile");
    }

    if (bytes_read == 0) {
        out.clear();
        return out;
    }

    out.resize(
        static_cast<std::size_t>(bytes_read)
    );

    return out;
}

const headmotion::transport::SerialConfig&
WindowsSerialPort::config() const {
    return config_;
}

void WindowsSerialPort::configurePort() {
    ensureOpen();

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);

    if (!GetCommState(handle_, &dcb)) {
        throwWindowsError("GetCommState");
    }

    dcb.BaudRate = config_.baud_rate;
    dcb.ByteSize = config_.data_bits;
    dcb.fBinary = TRUE;
    dcb.fAbortOnError = FALSE;

    switch (config_.parity) {
        case headmotion::transport::Parity::None:
            dcb.Parity = NOPARITY;
            dcb.fParity = FALSE;
            break;

        case headmotion::transport::Parity::Even:
            dcb.Parity = EVENPARITY;
            dcb.fParity = TRUE;
            break;

        case headmotion::transport::Parity::Odd:
            dcb.Parity = ODDPARITY;
            dcb.fParity = TRUE;
            break;
    }

    if (config_.stop_bits == 1) {
        dcb.StopBits = ONESTOPBIT;
    } else if (config_.stop_bits == 2) {
        dcb.StopBits = TWOSTOPBITS;
    } else {
        throw std::runtime_error(
            "Unsupported stop_bits value"
        );
    }

    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;

    dcb.fDtrControl =
        config_.assert_dtr
            ? DTR_CONTROL_ENABLE
            : DTR_CONTROL_DISABLE;

    switch (config_.flow_control) {
        case headmotion::transport::FlowControl::None:
            dcb.fOutxCtsFlow = FALSE;

            dcb.fRtsControl =
                config_.assert_rts
                    ? RTS_CONTROL_ENABLE
                    : RTS_CONTROL_DISABLE;
            break;

        case headmotion::transport::FlowControl::Hardware:
            dcb.fOutxCtsFlow = TRUE;
            dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
            break;
    }

    if (!SetCommState(handle_, &dcb)) {
        throwWindowsError("SetCommState");
    }
}

void WindowsSerialPort::configureTimeouts(
    std::chrono::milliseconds read_timeout
) {
    ensureOpen();

    COMMTIMEOUTS timeouts{};

    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant =
        checkedTimeout(read_timeout);

    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;

    if (!SetCommTimeouts(handle_, &timeouts)) {
        throwWindowsError("SetCommTimeouts");
    }
}

void WindowsSerialPort::ensureOpen() const {
    if (handle_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "WindowsSerialPort is not open"
        );
    }
}

} // namespace headmotion::platform::windows_platform
