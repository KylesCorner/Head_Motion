#pragma once

#include <string>

namespace headmotion {

struct BleDeviceInfo {
    std::string identifier;
    std::string address;
    int16_t rssi = 0;
};

}  // namespace headmotion
