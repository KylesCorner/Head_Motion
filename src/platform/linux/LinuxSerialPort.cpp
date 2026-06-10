#include "LinuxSerialPort.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace headmotion::platform::linux_platform {

namespace {

std::string errnoMessage(const std::string& operation) {
    return operation + " failed: " + std::strerror(errno);
}

void throwErrno(const std::string& operation) {
    throw std::runtime_error(errnoMessage(operation));
}

} // namespace

LinuxSerialPort::LinuxSerialPort(headmotion::transport::SerialConfig config)
    : config_(std::move(config)) {}

LinuxSerialPort::~LinuxSerialPort() {
    close();
}

LinuxSerialPort::LinuxSerialPort(LinuxSerialPort&& other) noexcept
    : config_(std::move(other.config_)),
      fd_(std::exchange(other.fd_, -1)) {}

LinuxSerialPort& LinuxSerialPort::operator=(LinuxSerialPort&& other) noexcept {
    if (this != &other) {
        close();

        config_ = std::move(other.config_);
        fd_ = std::exchange(other.fd_, -1);
    }

    return *this;
}

void LinuxSerialPort::open() {
    close();

    if (config_.port_name.empty()) {
        throw std::runtime_error("Serial port name is empty");
    }

    fd_ = ::open(
        config_.port_name.c_str(),
        O_RDWR | O_NOCTTY | O_NONBLOCK
    );

    if (fd_ < 0) {
        throwErrno("open(" + config_.port_name + ")");
    }

    try {
        configurePort();
        configureDtrRts();

        if (config_.open_delay.count() > 0) {
            std::this_thread::sleep_for(config_.open_delay);
        }

        tcflush(fd_, TCIOFLUSH);
    } catch (...) {
        close();
        throw;
    }
}

void LinuxSerialPort::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool LinuxSerialPort::isOpen() const {
    return fd_ >= 0;
}

void LinuxSerialPort::write(const std::vector<std::uint8_t>& bytes) {
    ensureOpen();

    if (bytes.empty()) {
        return;
    }

    std::size_t offset = 0;

    while (offset < bytes.size()) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;

        const int poll_result = ::poll(&pfd, 1, 1000);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            throwErrno("poll(POLLOUT)");
        }

        if (poll_result == 0) {
            throw std::runtime_error("Serial write timed out");
        }

        if ((pfd.revents & POLLOUT) == 0) {
            throw std::runtime_error("Serial port not ready for write");
        }

        const ssize_t n = ::write(
            fd_,
            bytes.data() + offset,
            bytes.size() - offset
        );

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            throwErrno("write");
        }

        if (n == 0) {
            throw std::runtime_error("Serial write returned zero bytes");
        }

        offset += static_cast<std::size_t>(n);
    }

    if (tcdrain(fd_) != 0) {
        throwErrno("tcdrain");
    }
}

std::vector<std::uint8_t> LinuxSerialPort::read(
    std::size_t max_bytes,
    std::chrono::milliseconds timeout
) {
    ensureOpen();

    std::vector<std::uint8_t> out;

    if (max_bytes == 0) {
        return out;
    }

    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    const int poll_timeout_ms = static_cast<int>(timeout.count());

    while (true) {
        const int poll_result = ::poll(&pfd, 1, poll_timeout_ms);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            throwErrno("poll(POLLIN)");
        }

        if (poll_result == 0) {
            return out;
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("Serial port error/hangup while reading");
        }

        if ((pfd.revents & POLLIN) == 0) {
            return out;
        }

        break;
    }

    out.resize(max_bytes);

    while (true) {
        const ssize_t n = ::read(fd_, out.data(), out.size());

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                out.clear();
                return out;
            }

            throwErrno("read");
        }

        if (n == 0) {
            out.clear();
            return out;
        }

        out.resize(static_cast<std::size_t>(n));
        return out;
    }
}

const headmotion::transport::SerialConfig& LinuxSerialPort::config() const {
    return config_;
}

void LinuxSerialPort::configurePort() {
    ensureOpen();

    termios tty{};

    if (tcgetattr(fd_, &tty) != 0) {
        throwErrno("tcgetattr");
    }

    cfmakeraw(&tty);

    tty.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);

    tty.c_cflag &= static_cast<tcflag_t>(~CSIZE);

    switch (config_.data_bits) {
        case 5:
            tty.c_cflag |= CS5;
            break;
        case 6:
            tty.c_cflag |= CS6;
            break;
        case 7:
            tty.c_cflag |= CS7;
            break;
        case 8:
            tty.c_cflag |= CS8;
            break;
        default:
            throw std::runtime_error("Unsupported data_bits value");
    }

    switch (config_.parity) {
        case headmotion::transport::Parity::None:
            tty.c_cflag &= static_cast<tcflag_t>(~PARENB);
            break;

        case headmotion::transport::Parity::Even:
            tty.c_cflag |= PARENB;
            tty.c_cflag &= static_cast<tcflag_t>(~PARODD);
            break;

        case headmotion::transport::Parity::Odd:
            tty.c_cflag |= PARENB;
            tty.c_cflag |= PARODD;
            break;
    }

    if (config_.stop_bits == 1) {
        tty.c_cflag &= static_cast<tcflag_t>(~CSTOPB);
    } else if (config_.stop_bits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        throw std::runtime_error("Unsupported stop_bits value");
    }

    switch (config_.flow_control) {
        case headmotion::transport::FlowControl::None:
#ifdef CRTSCTS
            tty.c_cflag &= static_cast<tcflag_t>(~CRTSCTS);
#endif
            tty.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));
            break;

        case headmotion::transport::FlowControl::Hardware:
#ifdef CRTSCTS
            tty.c_cflag |= CRTSCTS;
#else
            throw std::runtime_error("Hardware flow control is not supported on this platform");
#endif
            tty.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY));
            break;
    }

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    const speed_t speed = baudToSpeed(config_.baud_rate);

    if (cfsetispeed(&tty, speed) != 0) {
        throwErrno("cfsetispeed");
    }

    if (cfsetospeed(&tty, speed) != 0) {
        throwErrno("cfsetospeed");
    }

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        throwErrno("tcsetattr");
    }
}

void LinuxSerialPort::configureDtrRts() {
    ensureOpen();

    int status = 0;

    if (ioctl(fd_, TIOCMGET, &status) != 0) {
        throwErrno("ioctl(TIOCMGET)");
    }

    if (config_.assert_dtr) {
        status |= TIOCM_DTR;
    } else {
        status &= ~TIOCM_DTR;
    }

    if (config_.assert_rts) {
        status |= TIOCM_RTS;
    } else {
        status &= ~TIOCM_RTS;
    }

    if (ioctl(fd_, TIOCMSET, &status) != 0) {
        throwErrno("ioctl(TIOCMSET)");
    }
}

void LinuxSerialPort::ensureOpen() const {
    if (fd_ < 0) {
        throw std::runtime_error("LinuxSerialPort is not open");
    }
}

speed_t LinuxSerialPort::baudToSpeed(std::uint32_t baud_rate) {
    switch (baud_rate) {
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        case 230400:
            return B230400;
        case 460800:
            return B460800;
        case 500000:
            return B500000;
        case 576000:
            return B576000;
        case 921600:
            return B921600;
        case 1000000:
            return B1000000;
        case 1152000:
            return B1152000;
        case 1500000:
            return B1500000;
        case 2000000:
            return B2000000;
        default:
            throw std::runtime_error("Unsupported baud rate: " + std::to_string(baud_rate));
    }
}

} // namespace headmotion::platform::linux_platform