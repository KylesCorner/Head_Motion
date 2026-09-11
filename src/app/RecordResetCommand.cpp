/*
 * RecordResetCommand
 * ------------------
 *
 * Performs destructive board-side cleanup for a MetaMotionS/MMS+ recording
 * session.
 *
 * Stateless host behavior:
 *
 *   - Uses a fresh SDK board initialized over USB.
 *   - Stops IMU sampling and onboard logging.
 *   - Clears logged flash entries.
 *   - Removes recorded event commands.
 *   - Tears down board routes/loggers/events/timers known to the SDK.
 *   - Erases macros.
 *   - Resets the board after garbage collection.
 *   - Does not read, write, or delete any host-side board state.
 *
 * This command is intentionally destructive to the board recording state.
 */

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
#include <iostream>
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

            while (std::chrono::steady_clock::now() < deadline) {
                bridge.pumpOnce(50);

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10)
                );
            }
        }

        void stopGyroIfPresent(
            headmotion::sdk::MetaWearSdkBridge& bridge,
            MblMwMetaWearBoard* board
        ) {
            const std::int32_t impl =
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

        headmotion::transport::SerialConfig config;
        config.port_name = port_name;
        config.baud_rate = 115200;
        config.data_bits = 8;
        config.stop_bits = 1;
        config.assert_dtr = true;
        config.assert_rts = true;
        config.open_delay = 100ms;

        auto serial =
            headmotion::transport::SerialPortFactory::create(
                config
            );

        headmotion::metawear::MetaWearUsbTransport usb(
            *serial
        );

        std::cout
            << "Opening "
            << port_name
            << "\n";

        usb.open();

        headmotion::sdk::MetaWearSdkBridge bridge(usb);

        std::cout
            << "Initializing fresh SDK board over USB\n";

        const bool initialized =
            bridge.initialize(5000);

        if (!initialized) {
            std::cerr
                << "SDK init failed, status="
                << bridge.initializeStatus()
                << "\n";

            return 2;
        }

        MblMwMetaWearBoard* board =
            bridge.board();

        if (board == nullptr) {
            std::cerr
                << "SDK returned a null board\n";

            return 3;
        }

        /*
         * Stop data producers first.
         */
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

        /*
         * Stop flash logging before destructive cleanup.
         */
        std::cout
            << "Stopping internal logging\n";

        mbl_mw_logging_stop(board);
        pumpFor(bridge, 500);

        /*
         * Remove board-side event commands.  This is important for the stateless
         * battery timer path: record-stop cannot recover the timer object itself
         * from a fresh SDK instance, but reset can remove its recorded commands
         * before resetting the board.
         */
        std::cout
            << "Removing recorded event commands\n";

        mbl_mw_event_remove_all(board);
        pumpFor(bridge, 500);

        /*
         * Delete the accumulated flash log data.
         */
        std::cout
            << "Clearing logged entries\n";

        mbl_mw_logging_clear_entries(board);
        pumpFor(bridge, 1500);

        /*
         * Tear down logging routes, processors, events, and timers represented by
         * the current SDK board.  No serialized host state is involved.
         */
        std::cout
            << "Tearing down board routes/loggers/events/timers\n";

        mbl_mw_metawearboard_tear_down(board);
        pumpFor(bridge, 1500);

        std::cout
            << "Erasing macros\n";

        mbl_mw_macro_erase_all(board);
        pumpFor(bridge, 500);

        /*
         * The reset is the final board-side cleanup step.  The USB serial device
         * can disappear and re-enumerate while this happens.
         */
        std::cout
            << "Resetting board after garbage collection\n"
            << "The USB serial device may disconnect/reconnect now.\n";

        mbl_mw_debug_reset_after_gc(board);

        /*
         * Best effort only: once reset occurs, the transport may disappear.
         */
        pumpFor(bridge, 3000);

        std::cout
            << "Record reset complete.\n"
            << "No host-side board state or logger metadata "
            << "was read, written, or removed.\n"
            << "Wait a few seconds for the MMS+ USB serial device "
            << "to reappear, then run record-start.\n";

        return 0;
    }

} // namespace headmotion::app
