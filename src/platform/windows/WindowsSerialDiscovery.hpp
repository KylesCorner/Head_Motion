#pragma once

#include "headmotion/transport/SerialPortInfo.hpp"

#include <string>
#include <vector>

namespace headmotion::platform::windows_platform {

class WindowsSerialDiscovery {
public:
    std::vector<headmotion::transport::SerialPortInfo> listPorts() const;

    static bool looksLikeMmsName(const std::string& value);
};

} // namespace headmotion::platform::windows_platform
