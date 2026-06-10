#pragma once

#include "headmotion/transport/IByteTransport.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortInfo.hpp"

#include <memory>
#include <vector>

namespace headmotion::transport {

class SerialPortFactory {
public:
    static std::unique_ptr<IByteTransport> create(const SerialConfig& config);
    static std::vector<SerialPortInfo> listPorts();
};

} // namespace headmotion::transport