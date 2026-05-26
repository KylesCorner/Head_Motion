#pragma once

#include "headmotion/interfaces/IBleAdapter.h"

#include <vector>

namespace headmotion::test {

class FakeBleAdapter : public IBleAdapter {
public:
    bool scanCalled = false;

    Result scanResult = Result::success();

    std::vector<BleDeviceInfo> devicesToReturn;

    Result scan(std::vector<BleDeviceInfo>& devicesOut) override {
        scanCalled = true;

        if (!scanResult.ok()) {
            return scanResult;
        }

        devicesOut = devicesToReturn;
        return Result::success();
    }
};

}  // namespace headmotion::test
