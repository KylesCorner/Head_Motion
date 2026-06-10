#pragma once

#include "headmotion/transport/SerialPortInfo.hpp"

#include <vector>

namespace headmotion::platform::linux_platform {

class LinuxSerialDiscovery {
public:
    std::vector<headmotion::transport::SerialPortInfo> listPorts() const;

private:
    static std::vector<headmotion::transport::SerialPortInfo> listGlobbedDevices();
    static std::vector<headmotion::transport::SerialPortInfo> listByIdDevices();

    static bool looksLikeMmsName(const std::string& value);
    static std::string canonicalPath(const std::string& path);
};

} // namespace headmotion::platform::linux_platform