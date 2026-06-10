#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/logging.h"
#include "metawear/core/metawearboard.h"
#include "metawear/sensor/accelerometer.h"
#include "metawear/sensor/gyro_bosch.h"
}

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace headmotion::app {

namespace {

void pumpFor(headmotion::sdk::MetaWearSdkBridge& bridge, int total_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int runRecordResetCommand(const std::string& port_name) {
    using namespace std::chrono_literals;

    const auto state_path = headmotion::session::BoardStateStore::defaultPath();

    if (std::filesystem::exists(state_path)) {
        std::filesystem::remove(state_path);
        std::cout << "Removed stale board state: " << state_path << "\n";
    }

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

    std::cout << "Stopping gyro sampling\n";
    mbl_mw_gyro_bmi160_stop(board);
    pumpFor(bridge, 150);

    std::cout << "Disabling gyro rotation sampling\n";
    mbl_mw_gyro_bmi160_disable_rotation_sampling(board);
    pumpFor(bridge, 150);

    std::cout << "Stopping accelerometer sampling\n";
    mbl_mw_acc_stop(board);
    pumpFor(bridge, 150);

    std::cout << "Disabling accelerometer sampling\n";
    mbl_mw_acc_disable_acceleration_sampling(board);
    pumpFor(bridge, 150);

    std::cout << "Stopping internal logging\n";
    mbl_mw_logging_stop(board);
    pumpFor(bridge, 250);

    std::cout << "Clearing logged entries\n";
    mbl_mw_logging_clear_entries(board);
    pumpFor(bridge, 500);

    std::cout << "Tearing down board routes/loggers/events\n";
    mbl_mw_metawearboard_tear_down(board);
    pumpFor(bridge, 1000);

    std::cout << "Record reset complete.\n";
    std::cout << "Board logger routes should now be cleared.\n";
    std::cout << "You can now run record-start again.\n";

    return 0;
}

} // namespace headmotion::app