#pragma once

#include "headmotion/interfaces/IBleAdapter.h"

namespace headmotion {

class SimpleBleAdapter : public IBleAdapter {
public:
    Result scan(std::vector<BleDeviceInfo>& devicesOut) override;
};

}  // namespace headmotion
