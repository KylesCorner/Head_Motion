#pragma once

#include "headmotion/core/BleDeviceInfo.h"
#include "headmotion/core/Result.h"
#include "headmotion/interfaces/IBleAdapter.h"
#include "headmotion/interfaces/ILogger.h"

#include <vector>

namespace headmotion {

class ScanService {
public:
    ScanService(IBleAdapter& ble, ILogger& logger)
        : ble_(ble),
          logger_(logger) {}

    Result run(std::vector<BleDeviceInfo>& devicesOut) {
        devicesOut.clear();

        Result r = ble_.scan(devicesOut);
        if (!r.ok()) {
            logger_.error("BLE scan failed: " + r.message());
            return r;
        }

        logger_.info("BLE scan found " + std::to_string(devicesOut.size()) + " device(s)");
        return Result::success();
    }

private:
    IBleAdapter& ble_;
    ILogger& logger_;
};

}  // namespace headmotion
