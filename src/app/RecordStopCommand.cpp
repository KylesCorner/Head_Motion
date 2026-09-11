/*
 * RecordStopCommand
 * -----------------
 *
 * Stops an internal recording session on the MetaMotionS/MMS+ sensor.
 *
 * Stateless host behavior:
 *
 *   - Initializes a fresh SDK board over USB.
 *   - Stops onboard logging.
 *   - Stops and disables accelerometer sampling.
 *   - Stops and disables gyroscope sampling.
 *   - Removes recorded event commands so an existing battery timer can no
 *     longer trigger battery reads.
 *   - Does not load serialized SDK board state.
 *   - Does not load logger metadata or timer IDs from the host.
 *
 * Note:
 * The public MetaWear SDK cannot reconstruct an existing MblMwTimer object
 * from a fresh SDK instance.  Therefore record-stop removes the timer's
 * recorded event commands but leaves final timer removal to record-reset.
 */

#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/event.h"
#include "metawear/core/logging.h"
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

    int runRecordStopCommand(
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
         * Stop logging first.  This prevents any further sensor or battery
         * samples from being committed to flash while the individual producers
         * are being shut down.
         */
        std::cout
            << "Stopping internal logging\n";

        mbl_mw_logging_stop(board);
        pumpFor(bridge, 250);

        /*
         * Stop the high-rate sensors explicitly.
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
         * The battery logger created by record-start is driven by a board timer
         * whose event contains a battery-read command.  A fresh SDK instance
         * cannot recover the existing MblMwTimer object, so there is no supported
         * public API call that can stop that specific timer without persisted SDK
         * state.
         *
         * Removing all recorded event commands makes that timer inert: it may
         * continue to fire internally, but it no longer performs battery reads.
         * record-reset will perform the destructive cleanup of board routes,
         * timers, and log contents.
         */
        std::cout
            << "Removing recorded event commands\n";

        mbl_mw_event_remove_all(board);
        pumpFor(bridge, 250);

        std::cout
            << "Record stop complete.\n"
            << "No host-side board state or logger metadata "
            << "was read or written.\n"
            << "Accel + gyro sampling is stopped.\n"
            << "Internal logging is stopped.\n"
            << "Battery timer event commands were removed.\n"
            << "Logs were not cleared; run sync/download next.\n";

        return 0;
    }

} // namespace headmotion::app
