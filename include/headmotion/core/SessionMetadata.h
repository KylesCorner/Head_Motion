#pragma once

#include <cstdint>
#include <string>

namespace headmotion {

struct SessionMetadata {
    std::string sessionId;

    std::string deviceMac;
    std::string serialNumber;
    std::string firmwareRevision;

    std::string manufacturer;
    std::string modelNumber;
    std::string hardwareRevision;

    uint8_t accelLoggerId = 0xff;
    uint8_t gyroLoggerId = 0xff;

    float accelHz = 50.0f;
    float gyroHz = 50.0f;

    int64_t startedEpochMs = 0;
};

}  // namespace headmotion
