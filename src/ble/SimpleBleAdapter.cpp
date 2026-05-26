#include "headmotion/ble/SimpleBleAdapter.h"

#include <simpleble/SimpleBLE.h>

#include <exception>
#include <string>

namespace headmotion {

Result SimpleBleAdapter::scan(std::vector<BleDeviceInfo>& devicesOut) {
    devicesOut.clear();

    try {
        if (!SimpleBLE::Adapter::bluetooth_enabled()) {
            return Result::error("Bluetooth is not enabled");
        }

        auto adapters = SimpleBLE::Adapter::get_adapters();
        if (adapters.empty()) {
            return Result::error("No Bluetooth adapters found");
        }

        auto adapter = adapters.at(0);

        adapter.scan_for(3000);

        auto peripherals = adapter.scan_get_results();

        for (auto& peripheral : peripherals) {
            BleDeviceInfo info;
            info.identifier = peripheral.identifier();
            info.address = peripheral.address();
            info.rssi = peripheral.rssi();

            devicesOut.push_back(info);
        }

        return Result::success();
    } catch (const std::exception& e) {
        return Result::error(std::string("SimpleBLE scan failed: ") + e.what());
    }
}

}  // namespace headmotion
