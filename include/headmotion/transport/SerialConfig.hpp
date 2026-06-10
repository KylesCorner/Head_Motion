#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace headmotion::transport {

enum class Parity {
    None,
    Even,
    Odd
};

enum class FlowControl {
    None,
    Hardware
};

struct SerialConfig {
    std::string port_name;

    std::uint32_t baud_rate = 115200;
    std::uint8_t data_bits = 8;
    std::uint8_t stop_bits = 1;

    Parity parity = Parity::None;
    FlowControl flow_control = FlowControl::None;

    bool assert_dtr = true;
    bool assert_rts = true;

    std::chrono::milliseconds open_delay{100};
};

} // namespace headmotion::transport