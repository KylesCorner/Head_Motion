#pragma once

#include "headmotion/transport/IByteTransport.hpp"
#include "headmotion/transport/SerialConfig.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <termios.h>

namespace headmotion::platform::linux_platform {

class LinuxSerialPort final : public headmotion::transport::IByteTransport {
public:
    explicit LinuxSerialPort(headmotion::transport::SerialConfig config);
    ~LinuxSerialPort() override;

    LinuxSerialPort(const LinuxSerialPort&) = delete;
    LinuxSerialPort& operator=(const LinuxSerialPort&) = delete;

    LinuxSerialPort(LinuxSerialPort&& other) noexcept;
    LinuxSerialPort& operator=(LinuxSerialPort&& other) noexcept;

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
    int fd_ = -1;

    void configurePort();
    void configureDtrRts();

    void ensureOpen() const;

    static speed_t baudToSpeed(std::uint32_t baud_rate);
};

} // namespace headmotion::platform::linux_platform