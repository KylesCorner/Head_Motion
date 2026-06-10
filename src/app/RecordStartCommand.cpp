#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/datasignal.h"
#include "metawear/core/logging.h"
#include "metawear/sensor/accelerometer.h"
#include "metawear/sensor/gyro_bosch.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace headmotion::app {

namespace {

struct LoggerCreateState {
    std::atomic<int> callbacks{0};
    std::atomic<int> failures{0};

    MblMwDataLogger* accel_logger = nullptr;
    MblMwDataLogger* gyro_logger = nullptr;
};

void validateSampleRate(float sample_rate_hz) {
    if (sample_rate_hz != 25.0f &&
        sample_rate_hz != 50.0f &&
        sample_rate_hz != 100.0f) {
        throw std::runtime_error("Unsupported sample rate. Use one of: 25, 50, 100 Hz");
    }
}

MblMwGyroBoschOdr gyroOdrFromRate(float sample_rate_hz) {
    if (sample_rate_hz == 25.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_25Hz;
    }

    if (sample_rate_hz == 50.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_50Hz;
    }

    if (sample_rate_hz == 100.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_100Hz;
    }

    throw std::runtime_error("Unsupported sample rate. Use one of: 25, 50, 100 Hz");
}

void removeStaleBoardState() {
    const auto state_path = headmotion::session::BoardStateStore::defaultPath();

    if (std::filesystem::exists(state_path)) {
        std::filesystem::remove(state_path);
        std::cout << "Removed stale board state: " << state_path << "\n";
    }
}

void onAccelLoggerCreated(void* context, MblMwDataLogger* logger) {
    auto* state = static_cast<LoggerCreateState*>(context);

    if (logger == nullptr) {
        std::cout << "Accel logger creation failed\n";
        state->failures++;
    } else {
        std::cout << "Accel logger created\n";
        state->accel_logger = logger;
    }

    state->callbacks++;
}

void onGyroLoggerCreated(void* context, MblMwDataLogger* logger) {
    auto* state = static_cast<LoggerCreateState*>(context);

    if (logger == nullptr) {
        std::cout << "Gyro logger creation failed\n";
        state->failures++;
    } else {
        std::cout << "Gyro logger created\n";
        state->gyro_logger = logger;
    }

    state->callbacks++;
}

bool waitForLoggerCallbacks(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    LoggerCreateState& state,
    int expected_callbacks,
    int timeout_ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(100);

        if (state.callbacks.load() >= expected_callbacks) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return false;
}

void pumpFor(headmotion::sdk::MetaWearSdkBridge& bridge, int total_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int runRecordStartCommand(const std::string& port_name, float sample_rate_hz) {
    using namespace std::chrono_literals;

    validateSampleRate(sample_rate_hz);

    /*
     * Important:
     *
     * A failed record-start must not leave an old state file behind.
     * Sync depends on this file containing the logger routes from the
     * current successful record-start.
     */
    removeStaleBoardState();

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

    std::cout << "Configuring accelerometer: "
              << sample_rate_hz
              << " Hz, +/-4 g\n";

    mbl_mw_acc_set_odr(board, sample_rate_hz);
    mbl_mw_acc_set_range(board, 4.0f);
    mbl_mw_acc_write_acceleration_config(board);
    pumpFor(bridge, 250);

    std::cout << "Configuring gyro BMI160: "
              << sample_rate_hz
              << " Hz, +/-500 dps\n";

    mbl_mw_gyro_bmi160_set_odr(board, gyroOdrFromRate(sample_rate_hz));
    mbl_mw_gyro_bmi160_set_range(board, MBL_MW_GYRO_BOSCH_RANGE_500dps);
    mbl_mw_gyro_bmi160_write_config(board);
    pumpFor(bridge, 250);

    MblMwDataSignal* accel_signal =
        mbl_mw_acc_get_acceleration_data_signal(board);

    MblMwDataSignal* gyro_signal =
        mbl_mw_gyro_bmi160_get_rotation_data_signal(board);

    if (accel_signal == nullptr) {
        std::cerr << "Failed to get accelerometer data signal\n";
        return 3;
    }

    if (gyro_signal == nullptr) {
        std::cerr << "Failed to get gyro data signal\n";
        return 3;
    }

    LoggerCreateState logger_state;

    std::cout << "Creating accelerometer logger\n";
    mbl_mw_datasignal_log(
        accel_signal,
        &logger_state,
        onAccelLoggerCreated
    );

    if (!waitForLoggerCallbacks(bridge, logger_state, 1, 5000)) {
        std::cerr << "Timed out waiting for accelerometer logger creation\n";
        return 4;
    }

    std::cout << "Creating gyro logger\n";
    mbl_mw_datasignal_log(
        gyro_signal,
        &logger_state,
        onGyroLoggerCreated
    );

    if (!waitForLoggerCallbacks(bridge, logger_state, 2, 5000)) {
        std::cerr << "Timed out waiting for gyro logger creation\n";
        return 4;
    }

    if (logger_state.failures.load() != 0) {
        std::cerr << "One or more logger creations failed\n";
        std::cerr << "The board probably still has old logger routes allocated.\n";
        std::cerr << "Run record-reset before starting a fresh session.\n";
        return 5;
    }

    std::cout << "Starting internal logging, overwrite=false\n";
    mbl_mw_logging_start(board, 0);
    pumpFor(bridge, 250);

    std::cout << "Starting accelerometer sampling\n";
    mbl_mw_acc_enable_acceleration_sampling(board);
    mbl_mw_acc_start(board);
    pumpFor(bridge, 250);

    std::cout << "Starting gyro sampling\n";
    mbl_mw_gyro_bmi160_enable_rotation_sampling(board);
    mbl_mw_gyro_bmi160_start(board);
    pumpFor(bridge, 250);

    const auto board_state = bridge.serializeBoard();
    const auto state_path = headmotion::session::BoardStateStore::defaultPath();

    headmotion::session::BoardStateStore::save(state_path, board_state);

    std::cout << "Saved board state: "
              << state_path
              << " ["
              << board_state.size()
              << " bytes]\n";

    std::cout << "Record start complete.\n";
    std::cout << "Sample rate: " << sample_rate_hz << " Hz\n";
    std::cout << "The MMS should now be internally logging accel + gyro.\n";
    std::cout << "Use record-stop before sync/download.\n";

    return 0;
}

} // namespace headmotion::app