#pragma once

#include <cstdint>

namespace headmotion {

struct ImuSample {
    enum class Kind {
        Accel,
        Gyro,
        Quaternion
    };

    Kind kind = Kind::Accel;

    int64_t epochMs = 0;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

}  // namespace headmotion
