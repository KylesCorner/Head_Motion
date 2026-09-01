#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/debug.h"
#include "metawear/core/logging.h"
#include "metawear/core/macro.h"
#include "metawear/core/metawearboard.h"
#include "metawear/core/module.h"
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

constexpr int32_t GYRO_IMPL_BMI160 = 0;
constexpr int32_t GYRO_IMPL_BMI270 = 1;

void pumpFor(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    int total_ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(total_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(50);

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }
}

void removeLocalStateFiles(
    const std::filesystem::path& state_path,
    const std::filesystem::path& metadata_path
) {
    if (std::filesystem::exists(state_path)) {
        std::filesystem::remove(state_path);

        std::cout
            << "Removed board state: "
            << state_path
            << "\n";
    }

    if (std::filesystem::exists(metadata_path)) {
        std::filesystem::remove(metadata_path);

        std::cout
            << "Removed logger metadata: "
            << metadata_path
            << "\n";
    }
}

void stopGyroIfPresent(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board
) {
    const int32_t impl =
        mbl_mw_metawearboard_lookup_module(
            board,
            MBL_MW_MODULE_GYRO
        );

    if (impl < 0) {
        std::cout
            << "Gyro module not present; "
            << "skipping gyro stop\n";

        return;
    }

    std::cout
        << "Raw gyro implementation value: "
        << impl
        << "\n";

    if (impl == GYRO_IMPL_BMI160) {
        std::cout
            << "Stopping BMI160 gyro sampling\n";

        mbl_mw_gyro_bmi160_stop(board);
        pumpFor(bridge, 150);

        std::cout
            << "Disabling BMI160 gyro rotation sampling\n";

        mbl_mw_gyro_bmi160_disable_rotation_sampling(
            board
        );

        pumpFor(bridge, 150);

        return;
    }

    if (impl == GYRO_IMPL_BMI270) {
        std::cout
            << "Stopping BMI270 gyro sampling\n";

        mbl_mw_gyro_bmi270_stop(board);
        pumpFor(bridge, 150);

        std::cout
            << "Disabling BMI270 gyro rotation sampling\n";

        mbl_mw_gyro_bmi270_disable_rotation_sampling(
            board
        );

        pumpFor(bridge, 150);

        return;
    }

    std::cout
        << "Unknown gyro implementation "
        << impl
        << "; skipping explicit gyro stop\n";
}

} // namespace

int runRecordResetCommand(
    const std::string& port_name
) {
    using namespace std::chrono_literals;

    /*
     * Resolve this physical MMS+ before doing anything destructive.
     *
     * The USB serial number is the persistent sensor identity.
     */
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

    /*
     * Remove only this sensor's host-side state.
     *
     * Other connected or previously-used MMS+ devices are untouched.
     */
    removeLocalStateFiles(
        state_path,
        metadata_path
    );

    headmotion::transport::SerialConfig config;

    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    auto serial =
        headmotion::transport::SerialPortFactory::
            create(config);

    headmotion::metawear::MetaWearUsbTransport usb(
        *serial
    );

    std::cout
        << "Opening "
        << port_name
        << "\n";

    usb.open();

    headmotion::sdk::MetaWearSdkBridge bridge(
        usb
    );

    std::cout
        << "Initializing SDK board over USB\n";

    const bool initialized =
        bridge.initialize(5000);

    if (!initialized) {
        std::cerr
            << "SDK init failed, status="
            << bridge.initializeStatus()
            << "\n";

        return 2;
    }

    auto* board =
        bridge.board();

    stopGyroIfPresent(
        bridge,
        board
    );

    std::cout
        << "Stopping accelerometer sampling\n";

    mbl_mw_acc_stop(board);
    pumpFor(bridge, 150);

    std::cout
        << "Disabling accelerometer sampling\n";

    mbl_mw_acc_disable_acceleration_sampling(
        board
    );

    pumpFor(bridge, 150);

    std::cout
        << "Stopping internal logging\n";

    mbl_mw_logging_stop(board);
    pumpFor(bridge, 500);

    std::cout
        << "Clearing logged entries\n";

    mbl_mw_logging_clear_entries(board);
    pumpFor(bridge, 1500);

    std::cout
        << "Tearing down board "
        << "routes/loggers/events/timers\n";

    mbl_mw_metawearboard_tear_down(board);
    pumpFor(bridge, 1500);

    std::cout
        << "Erasing macros\n";

    mbl_mw_macro_erase_all(board);
    pumpFor(bridge, 500);

    std::cout
        << "Resetting board after garbage collection\n";

    std::cout
        << "The USB serial device may "
        << "disconnect/reconnect now.\n";

    mbl_mw_debug_reset_after_gc(board);

    pumpFor(bridge, 3000);

    std::cout
        << "Record reset complete.\n";

    std::cout
        << "Device ID: "
        << device_id
        << "\n";

    std::cout
        << "Wait a few seconds for the MMS+ "
        << "USB serial device to reappear, "
        << "then run record-start.\n";

    return 0;
}

} // namespace headmotion::app