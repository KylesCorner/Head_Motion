#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/logging.h"
#include "metawear/core/metawearboard.h"
#include "metawear/core/module.h"
#include "metawear/core/timer.h"
#include "metawear/sensor/accelerometer.h"
#include "metawear/sensor/gyro_bosch.h"
}

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace headmotion::app {

namespace {

constexpr int32_t GYRO_IMPL_BMI160 = 0;
constexpr int32_t GYRO_IMPL_BMI270 = 1;

struct StopMetadata {
    bool battery_enabled = false;
    int battery_timer_id = -1;
};


void pumpFor(headmotion::sdk::MetaWearSdkBridge& bridge, int total_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool loadStopMetadata(const std::filesystem::path& path, StopMetadata& metadata) {
    std::ifstream in(path);

    if (!in) {
        std::cout << "Logger metadata not found: " << path << "\n";
        std::cout << "Battery timer stop will be skipped.\n";
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');

        if (pos == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);

        try {
            if (key == "battery_enabled") {
                metadata.battery_enabled = std::stoi(value) != 0;
            } else if (key == "battery_timer_id") {
                metadata.battery_timer_id = std::stoi(value);
            }
        } catch (const std::exception& ex) {
            std::cout << "Ignoring malformed metadata line: " << line << "\n";
            std::cout << "Parse error: " << ex.what() << "\n";
        }
    }

    return true;
}

bool restoreBoardStateIfAvailable(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    const std::filesystem::path& state_path
) {
    if (!std::filesystem::exists(state_path)) {
        std::cout
            << "Saved board state not found: "
            << state_path
            << "\n";

        std::cout
            << "Timer lookup may not be available.\n";

        return false;
    }

    try {
        std::cout
            << "Loading board state: "
            << state_path
            << "\n";

        const auto board_state =
            headmotion::session::BoardStateStore::load(
                state_path
            );

        std::cout
            << "Deserializing board state ["
            << board_state.size()
            << " bytes]\n";

        bridge.deserializeBoard(
            board_state
        );

        pumpFor(bridge, 250);

        return true;
    } catch (const std::exception& ex) {
        std::cout
            << "Failed to restore board state: "
            << ex.what()
            << "\n";

        std::cout
            << "Continuing with best-effort stop.\n";

        return false;
    }
}

void stopBatteryTimerIfKnown(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    const StopMetadata& metadata
) {
    if (!metadata.battery_enabled) {
        std::cout << "Battery logging was disabled; no battery timer to stop\n";
        return;
    }

    if (metadata.battery_timer_id < 0) {
        std::cout << "Battery logging was enabled, but battery_timer_id is missing\n";
        std::cout << "This is expected if the session was started before timer IDs were saved.\n";
        return;
    }

    std::cout << "Looking up battery timer ID: "
              << metadata.battery_timer_id
              << "\n";

    MblMwTimer* battery_timer =
        mbl_mw_timer_lookup_id(
            board,
            static_cast<std::uint8_t>(metadata.battery_timer_id)
        );

    if (battery_timer == nullptr) {
        std::cout << "Could not look up battery timer ID "
                  << metadata.battery_timer_id
                  << "; skipping battery timer stop\n";
        return;
    }

    std::cout << "Stopping battery timer\n";
    mbl_mw_timer_stop(battery_timer);
    pumpFor(bridge, 250);
}

void stopGyroIfPresent(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board
) {
    const int32_t impl =
        mbl_mw_metawearboard_lookup_module(board, MBL_MW_MODULE_GYRO);

    if (impl < 0) {
        std::cout << "Gyro module not present; skipping gyro stop\n";
        return;
    }

    std::cout << "Raw gyro implementation value: " << impl << "\n";

    if (impl == GYRO_IMPL_BMI160) {
        std::cout << "Stopping BMI160 gyro sampling\n";
        mbl_mw_gyro_bmi160_stop(board);
        pumpFor(bridge, 150);

        std::cout << "Disabling BMI160 gyro rotation sampling\n";
        mbl_mw_gyro_bmi160_disable_rotation_sampling(board);
        pumpFor(bridge, 150);
        return;
    }

    if (impl == GYRO_IMPL_BMI270) {
        std::cout << "Stopping BMI270 gyro sampling\n";
        mbl_mw_gyro_bmi270_stop(board);
        pumpFor(bridge, 150);

        std::cout << "Disabling BMI270 gyro rotation sampling\n";
        mbl_mw_gyro_bmi270_disable_rotation_sampling(board);
        pumpFor(bridge, 150);
        return;
    }

    std::cout << "Unknown gyro implementation "
              << impl
              << "; skipping explicit gyro stop\n";
}

} // namespace

int runRecordStopCommand(const std::string& port_name) {
    using namespace std::chrono_literals;

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    const std::string device_id =
        headmotion::session::BoardStateStore::
            deviceIdForPort(port_name);

    const std::filesystem::path state_path =
        headmotion::session::BoardStateStore::
            boardStatePath(device_id);

    const std::filesystem::path metadata_path =
        headmotion::session::BoardStateStore::
            loggerMetadataPath(device_id);

    std::cout
        << "MMS+ device ID: "
        << device_id
        << "\n";

    std::cout
        << "Board state: "
        << state_path
        << "\n";

    std::cout
        << "Logger metadata: "
        << metadata_path
        << "\n";


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

    std::cout << "Stopping internal logging immediately\n";
    mbl_mw_logging_stop(board);
    pumpFor(bridge, 250);

    StopMetadata metadata;
    loadStopMetadata(metadata_path, metadata);

    restoreBoardStateIfAvailable(bridge, state_path);
    board = bridge.board();

    stopBatteryTimerIfKnown(bridge, board, metadata);

    stopGyroIfPresent(bridge, board);

    std::cout << "Stopping accelerometer sampling\n";
    mbl_mw_acc_stop(board);
    pumpFor(bridge, 150);

    std::cout << "Disabling accelerometer sampling\n";
    mbl_mw_acc_disable_acceleration_sampling(board);
    pumpFor(bridge, 150);

    std::cout << "Record stop complete.\n";
    std::cout << "Accel + gyro sampling should now be stopped.\n";
    std::cout << "Battery timer should now be stopped if its metadata was available.\n";
    std::cout << "Internal logging is stopped.\n";
    std::cout << "Logs were not cleared; run sync/download next.\n";

    return 0;
}

} // namespace headmotion::app