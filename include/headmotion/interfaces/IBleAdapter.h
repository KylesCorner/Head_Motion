#pragma once

#include "headmotion/core/BleDeviceInfo.h"
#include "headmotion/core/Result.h"

#include <vector>

namespace headmotion {

class IBleAdapter {
public:
    virtual ~IBleAdapter() = default;

    virtual Result scan(std::vector<BleDeviceInfo>& devicesOut) = 0;
};

}  // namespace headmotion
