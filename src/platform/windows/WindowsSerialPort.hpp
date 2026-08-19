#pragma once

#include "headmotion/transport/IByteTransport.hpp"
#include "headmotion/transport/SerialConfig.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace headmotion::platform::windows_platform {

class WindowsSerialPort final : public headmotion::transport::IByteTransport {
public:
    explicit WindowsSerialPort(headmotion::transport::SerialConfig config);
    ~WindowsSerialPort() override;

    WindowsSerialPort(const WindowsSerialPort&) = delete;
    WindowsSerialPort& operator=(const WindowsSerialPort&) = delete;

    WindowsSerialPort(WindowsSerialPort&& other) noexcept;
    WindowsSerialPort& operator=(WindowsSerialPort&& other) noexcept;

    void open() override;
    void close() override;
    bool isOpen() const override;

    void write(const std::vector<std::uint8_t>& bytes) override;

    std::vector<std::uint8_t> read(
        std::size_t max_bytes,
        std::chrono::milliseconds timeout
    ) override;

    const headmotion::transport::SerialConfig& config() const;

private:
    headmotion::transport::SerialConfig config_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;

    void configurePort();
    void configureTimeouts(std::chrono::milliseconds read_timeout);
    void ensureOpen() const;
};

} // namespace headmotion::platform::windows_platform
