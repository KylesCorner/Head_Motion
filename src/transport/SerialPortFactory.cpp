#include "headmotion/transport/SerialPortFactory.hpp"

#if defined(HEADMOTION_PLATFORM_LINUX)

#include "platform/linux/LinuxSerialDiscovery.hpp"
#include "platform/linux/LinuxSerialPort.hpp"

#elif defined(HEADMOTION_PLATFORM_WINDOWS)

#include "platform/windows/WindowsSerialDiscovery.hpp"
#include "platform/windows/WindowsSerialPort.hpp"

#endif

#include <stdexcept>

namespace headmotion::transport {

std::unique_ptr<IByteTransport>
SerialPortFactory::create(const SerialConfig& config) {

#if defined(HEADMOTION_PLATFORM_LINUX)

    return std::make_unique<
        headmotion::platform::linux_platform::LinuxSerialPort
    >(config);

#elif defined(HEADMOTION_PLATFORM_WINDOWS)

    return std::make_unique<
        headmotion::platform::windows_platform::WindowsSerialPort
    >(config);

#else

    (void)config;

    throw std::runtime_error(
        "No serial backend compiled for this platform"
    );

#endif
}

std::vector<SerialPortInfo>
SerialPortFactory::listPorts() {

#if defined(HEADMOTION_PLATFORM_LINUX)

    headmotion::platform::linux_platform::
        LinuxSerialDiscovery discovery;

    return discovery.listPorts();

#elif defined(HEADMOTION_PLATFORM_WINDOWS)

    headmotion::platform::windows_platform::
        WindowsSerialDiscovery discovery;

    return discovery.listPorts();

#else

    throw std::runtime_error(
        "No serial discovery backend compiled for this platform"
    );

#endif
}

} // namespace headmotion::transport
