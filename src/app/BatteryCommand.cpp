#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/datasignal.h"
#include "metawear/core/settings.h"
#include "metawear/core/types.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace headmotion::app {

namespace {

struct BatteryReadState {
    std::atomic<bool> done{false};
    std::atomic<bool> received{false};
};

void onBatteryData(void* context, const MblMwData* data) {
    auto* state = static_cast<BatteryReadState*>(context);

    if (state == nullptr) {
        return;
    }

    if (data == nullptr || data->value == nullptr) {
        std::cerr << "Battery callback received empty data\n";
        state->done.store(true);
        return;
    }

    const auto* battery =
        static_cast<const MblMwBatteryState*>(data->value);

    std::cout << "Battery voltage: "
              << battery->voltage
              << " mV\n";

    std::cout << "Battery charge: "
              << static_cast<int>(battery->charge)
              << " %\n";

    state->received.store(true);
    state->done.store(true);
}

bool waitForBatteryRead(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    BatteryReadState& state,
    int timeout_ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(50);

        if (state.done.load()) {
            return state.received.load();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return false;
}

} // namespace

int runBatteryCommand(const std::string& port_name) {
    using namespace std::chrono_literals;

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    auto serial = headmotion::transport::SerialPortFactory::create(config);
    headmotion::metawear::MetaWearUsbTransport usb(*serial);

    std::cout << "Opening " << port_name << "\n";
    usb.open();

    headmotion::sdk::MetaWearSdkBridge bridge(usb);

    std::cout << "Initializing SDK board over USB\n";
    const bool initialized = bridge.initialize(5000);

    if (!initialized) {
        std::cerr << "SDK init failed, status=" << bridge.initializeStatus() << "\n";
        return 2;
    }

    auto* board = bridge.board();

    MblMwDataSignal* battery_signal =
        mbl_mw_settings_get_battery_state_data_signal(board);

    if (battery_signal == nullptr) {
        std::cerr << "Failed to get battery data signal\n";
        return 3;
    }

    BatteryReadState state;

    std::cout << "Reading battery state\n";

    mbl_mw_datasignal_subscribe(
        battery_signal,
        &state,
        onBatteryData
    );

    mbl_mw_datasignal_read(battery_signal);

    const bool got_battery = waitForBatteryRead(bridge, state, 5000);

    mbl_mw_datasignal_unsubscribe(battery_signal);

    if (!got_battery) {
        std::cerr << "Timed out waiting for battery data\n";
        return 4;
    }

    return 0;
}

} // namespace headmotion::app