#pragma once

namespace headmotion {

struct SessionConfig {
    float accelHz = 50.0f;
    float gyroHz = 50.0f;

    float accelRangeG = 16.0f;
    float gyroRangeDps = 2000.0f;

    bool eraseBeforeStart = false;
};

}  // namespace headmotion
