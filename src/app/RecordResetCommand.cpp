/*
 * RecordResetCommand
 * ------------------
 *
 * Performs destructive board-side cleanup for a MetaMotionS/MMS+ recording
 * session while keeping the host side stateless.
 */

#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/debug.h"
#include "metawear/core/event.h"
#include "metawear/core/logging.h"
#include "metawear/core/macro.h"
#include "metawear/core/metawearboard.h"
#include "metawear/core/module.h"
#include "metawear/sensor/accelerometer.h"
#include "metawear/sensor/gyro_bosch.h"
}

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace headmotion::app {

namespace {

constexpr std::int32_t GYRO_IMPL_BMI160 = 0;
constexpr std::int32_t GYRO_IMPL_BMI270 = 1;

void pumpFor(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    int total_ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(total_ms);

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        bridge.pumpOnce(50);

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }
}

void stopGyroIfPresent(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    CommandOutput& output
) {
    const std::int32_t impl =
        mbl_mw_metawearboard_lookup_module(
            board,
            MBL_MW_MODULE_GYRO
        );

    output.event(
        "gyro",
        {
            {"implementation", impl}
        }
    );

    if (impl < 0) {
        output.warning(
            "gyro_not_present",
            "Gyro module not present; skipping gyro stop"
        );

        return;
    }

    if (impl == GYRO_IMPL_BMI160) {
        output.status(
            "stopping_bmi160_gyro"
        );

        mbl_mw_gyro_bmi160_stop(board);
        pumpFor(bridge, 150);

        output.status(
            "disabling_bmi160_gyro"
        );

        mbl_mw_gyro_bmi160_disable_rotation_sampling(
            board
        );

        pumpFor(bridge, 150);
        return;
    }

    if (impl == GYRO_IMPL_BMI270) {
        output.status(
            "stopping_bmi270_gyro"
        );

        mbl_mw_gyro_bmi270_stop(board);
        pumpFor(bridge, 150);

        output.status(
            "disabling_bmi270_gyro"
        );

        mbl_mw_gyro_bmi270_disable_rotation_sampling(
            board
        );

        pumpFor(bridge, 150);
        return;
    }

    output.warning(
        "unknown_gyro_implementation",
        "Unknown gyro implementation; skipping explicit gyro stop"
    );
}

} // namespace

int runRecordResetCommand(
    const std::string& port_name,
    CommandOutput& output
) {
    using namespace std::chrono_literals;

    output.started(
        {
            {"port", port_name},
            {"destructive", true}
        }
    );

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    output.status(
        "opening_port"
    );

    auto serial =
        headmotion::transport::SerialPortFactory::
        create(config);

    headmotion::metawear::MetaWearUsbTransport usb(
        *serial
    );

    usb.open();

    headmotion::sdk::MetaWearSdkBridge bridge(
        usb
    );

    output.status(
        "initializing_sdk"
    );

    const bool initialized =
        bridge.initialize(5000);

    if (!initialized) {
        output.error(
            "sdk_init_failed",
            "SDK initialization failed"
        );

        output.event(
            "sdk_init_result",
            {
                {
                    "status",
                    bridge.initializeStatus()
                }
            }
        );

        output.completed(
            false,
            2
        );

        return 2;
    }

    MblMwMetaWearBoard* board =
        bridge.board();

    if (board == nullptr) {
        output.error(
            "null_board",
            "SDK returned a null board"
        );

        output.completed(
            false,
            3
        );

        return 3;
    }

    stopGyroIfPresent(
        bridge,
        board,
        output
    );

    output.status(
        "stopping_accelerometer"
    );

    mbl_mw_acc_stop(board);
    pumpFor(bridge, 150);

    output.status(
        "disabling_accelerometer"
    );

    mbl_mw_acc_disable_acceleration_sampling(
        board
    );

    pumpFor(bridge, 150);

    output.status(
        "stopping_internal_logging"
    );

    mbl_mw_logging_stop(board);
    pumpFor(bridge, 500);

    output.status(
        "removing_event_commands"
    );

    mbl_mw_event_remove_all(board);
    pumpFor(bridge, 500);

    output.status(
        "clearing_logged_entries"
    );

    mbl_mw_logging_clear_entries(board);
    pumpFor(bridge, 1500);

    output.status(
        "tearing_down_board_routes"
    );

    mbl_mw_metawearboard_tear_down(
        board
    );

    pumpFor(bridge, 1500);

    output.status(
        "erasing_macros"
    );

    mbl_mw_macro_erase_all(board);
    pumpFor(bridge, 500);

    output.status(
        "resetting_board"
    );

    output.warning(
        "usb_reenumeration_expected",
        "The MMS+ USB serial device may disconnect and reconnect during reset"
    );

    mbl_mw_debug_reset_after_gc(
        board
    );

    /*
     * Best effort only: the USB transport may disappear after reset.
     */
    pumpFor(bridge, 3000);

    output.event(
        "summary",
        {
            {"logging_stopped", true},
            {"logged_entries_cleared", true},
            {"event_commands_removed", true},
            {"board_routes_torn_down", true},
            {"macros_erased", true},
            {"board_reset_requested", true}
        }
    );

    output.completed(
        true,
        0
    );

    return 0;
}

int runRecordResetCommand(
    const std::string& port_name
) {
    CommandOutput output(
        "record-reset"
    );

    return runRecordResetCommand(
        port_name,
        output
    );
}

} // namespace headmotion::app
