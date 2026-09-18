/*
 * RecordStartCommand
 * ------------------
 *
 * Starts an internal recording session on the MetaMotionS/MMS+ sensor using
 * the USB serial transport instead of BLE.
 *
 * Stateless host behavior:
 *   - Creates logger routes directly on the MMS+.
 *   - Starts onboard logging and sensor sampling.
 *   - Optionally creates and starts an onboard battery timer/logger.
 *   - Does not serialize SDK board state.
 *   - Does not save logger IDs or recording metadata on the host.
 *
 * Output behavior:
 *   - All command events go through the per-operation CommandOutput.
 *   - No std::cout/std::cerr redirection is used.
 */

#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/datasignal.h"
#include "metawear/core/event.h"
#include "metawear/core/logging.h"
#include "metawear/core/metawearboard.h"
#include "metawear/core/module.h"
#include "metawear/core/settings.h"
#include "metawear/core/timer.h"
#include "metawear/core/types.h"
#include "metawear/sensor/accelerometer.h"
#include "metawear/sensor/gyro_bosch.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace headmotion::app {

namespace {

constexpr std::int32_t GYRO_IMPL_BMI160 = 0;
constexpr std::int32_t GYRO_IMPL_BMI270 = 1;

enum class GyroImpl {
    Unknown,
    Bmi160,
    Bmi270
};

struct LoggerCreateState {
    CommandOutput* output = nullptr;

    std::atomic<int> callbacks{ 0 };
    std::atomic<int> failures{ 0 };

    MblMwDataLogger* accel_logger = nullptr;
    MblMwDataLogger* gyro_logger = nullptr;
    MblMwDataLogger* battery_logger = nullptr;
};

struct TimerCreateState {
    CommandOutput* output = nullptr;

    std::atomic<bool> done{ false };
    std::atomic<bool> failed{ false };

    MblMwTimer* timer = nullptr;
};

struct EventRecordState {
    CommandOutput* output = nullptr;

    std::atomic<bool> done{ false };
    std::atomic<bool> failed{ false };
};

int fail(
    CommandOutput& output,
    const std::string& code,
    const std::string& message,
    int exit_code
) {
    output.error(
        code,
        message
    );

    output.completed(
        false,
        exit_code
    );

    return exit_code;
}

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

template <typename State>
bool waitForDone(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    State& state,
    int timeout_ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        bridge.pumpOnce(100);

        if (state.done.load()) {
            return
                !state.failed.load();
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }

    return false;
}

bool waitForLoggerCallbacks(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    LoggerCreateState& state,
    int expected_callbacks,
    int timeout_ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        bridge.pumpOnce(100);

        if (
            state.callbacks.load() >=
            expected_callbacks
        ) {
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }

    return false;
}

void emitModuleImpl(
    CommandOutput& output,
    MblMwMetaWearBoard* board,
    MblMwModule module,
    const char* name
) {
    const std::int32_t impl =
        mbl_mw_metawearboard_lookup_module(
            board,
            module
        );

    output.event(
        "module",
        {
            {"name", name},
            {"implementation", impl}
        }
    );
}

void emitCoreModuleInfo(
    CommandOutput& output,
    MblMwMetaWearBoard* board
) {
    emitModuleImpl(
        output,
        board,
        MBL_MW_MODULE_EVENT,
        "EVENT"
    );

    emitModuleImpl(
        output,
        board,
        MBL_MW_MODULE_LOGGING,
        "LOGGING"
    );

    emitModuleImpl(
        output,
        board,
        MBL_MW_MODULE_TIMER,
        "TIMER"
    );

    emitModuleImpl(
        output,
        board,
        MBL_MW_MODULE_SETTINGS,
        "SETTINGS"
    );

    emitModuleImpl(
        output,
        board,
        MBL_MW_MODULE_GYRO,
        "GYRO"
    );
}

void validateSampleRate(
    float sample_rate_hz
) {
    if (
        sample_rate_hz != 25.0f &&
        sample_rate_hz != 50.0f &&
        sample_rate_hz != 100.0f &&
        sample_rate_hz != 200.0f &&
        sample_rate_hz != 400.0f &&
        sample_rate_hz != 800.0f &&
        sample_rate_hz != 1600.0f &&
        sample_rate_hz != 3200.0f
    ) {
        throw std::runtime_error(
            "Unsupported sample rate. Use one of: "
            "25, 50, 100, 200, 400, 800, 1600, 3200 Hz"
        );
    }
}

MblMwGyroBoschOdr gyroOdrFromRate(
    float sample_rate_hz
) {
    if (sample_rate_hz == 25.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_25Hz;
    }

    if (sample_rate_hz == 50.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_50Hz;
    }

    if (sample_rate_hz == 100.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_100Hz;
    }

    if (sample_rate_hz == 200.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_200Hz;
    }

    if (sample_rate_hz == 400.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_400Hz;
    }

    if (sample_rate_hz == 800.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_800Hz;
    }

    if (sample_rate_hz == 1600.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_1600Hz;
    }

    if (sample_rate_hz == 3200.0f) {
        return
            MBL_MW_GYRO_BOSCH_ODR_3200Hz;
    }

    throw std::runtime_error(
        "Unsupported sample rate. Use one of: "
        "25, 50, 100, 200, 400, 800, 1600, 3200 Hz"
    );
}

GyroImpl detectGyroImpl(
    MblMwMetaWearBoard* board,
    CommandOutput& output
) {
    if (board == nullptr) {
        throw std::runtime_error(
            "Cannot detect gyro implementation: board is null"
        );
    }

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
        throw std::runtime_error(
            "Gyroscope module is not present on this board"
        );
    }

    if (impl == GYRO_IMPL_BMI160) {
        output.event(
            "gyro_detected",
            {
                {"model", "BMI160"}
            }
        );

        return GyroImpl::Bmi160;
    }

    if (impl == GYRO_IMPL_BMI270) {
        output.event(
            "gyro_detected",
            {
                {"model", "BMI270"}
            }
        );

        return GyroImpl::Bmi270;
    }

    throw std::runtime_error(
        "Unknown gyroscope implementation: " +
        std::to_string(impl)
    );
}

void configureGyro(
    MblMwMetaWearBoard* board,
    GyroImpl gyro_impl,
    float sample_rate_hz
) {
    const MblMwGyroBoschOdr odr =
        gyroOdrFromRate(
            sample_rate_hz
        );

    if (gyro_impl == GyroImpl::Bmi160) {
        mbl_mw_gyro_bmi160_set_odr(
            board,
            odr
        );

        mbl_mw_gyro_bmi160_set_range(
            board,
            MBL_MW_GYRO_BOSCH_RANGE_500dps
        );

        mbl_mw_gyro_bmi160_write_config(
            board
        );

        return;
    }

    if (gyro_impl == GyroImpl::Bmi270) {
        mbl_mw_gyro_bmi270_set_odr(
            board,
            odr
        );

        mbl_mw_gyro_bmi270_set_range(
            board,
            MBL_MW_GYRO_BOSCH_RANGE_500dps
        );

        mbl_mw_gyro_bmi270_write_config(
            board
        );

        return;
    }

    throw std::runtime_error(
        "Cannot configure gyro: unknown implementation"
    );
}

MblMwDataSignal* getGyroSignal(
    MblMwMetaWearBoard* board,
    GyroImpl gyro_impl
) {
    if (gyro_impl == GyroImpl::Bmi160) {
        return
            mbl_mw_gyro_bmi160_get_rotation_data_signal(
                board
            );
    }

    if (gyro_impl == GyroImpl::Bmi270) {
        return
            mbl_mw_gyro_bmi270_get_rotation_data_signal(
                board
            );
    }

    return nullptr;
}

void startGyro(
    MblMwMetaWearBoard* board,
    GyroImpl gyro_impl
) {
    if (gyro_impl == GyroImpl::Bmi160) {
        mbl_mw_gyro_bmi160_enable_rotation_sampling(
            board
        );

        mbl_mw_gyro_bmi160_start(
            board
        );

        return;
    }

    if (gyro_impl == GyroImpl::Bmi270) {
        mbl_mw_gyro_bmi270_enable_rotation_sampling(
            board
        );

        mbl_mw_gyro_bmi270_start(
            board
        );

        return;
    }

    throw std::runtime_error(
        "Cannot start gyro: unknown implementation"
    );
}

void onAccelLoggerCreated(
    void* context,
    MblMwDataLogger* logger
) {
    auto* state =
        static_cast<LoggerCreateState*>(
            context
        );

    if (state == nullptr) {
        return;
    }

    if (logger == nullptr) {
        if (state->output != nullptr) {
            state->output->error(
                "accel_logger_create_failed",
                "Accelerometer logger creation failed"
            );
        }

        state->failures++;
    }
    else {
        if (state->output != nullptr) {
            state->output->event(
                "logger_created",
                {
                    {"sensor", "acceleration"}
                }
            );
        }

        state->accel_logger = logger;
    }

    state->callbacks++;
}

void onGyroLoggerCreated(
    void* context,
    MblMwDataLogger* logger
) {
    auto* state =
        static_cast<LoggerCreateState*>(
            context
        );

    if (state == nullptr) {
        return;
    }

    if (logger == nullptr) {
        if (state->output != nullptr) {
            state->output->error(
                "gyro_logger_create_failed",
                "Gyro logger creation failed"
            );
        }

        state->failures++;
    }
    else {
        if (state->output != nullptr) {
            state->output->event(
                "logger_created",
                {
                    {"sensor", "gyroscope"}
                }
            );
        }

        state->gyro_logger = logger;
    }

    state->callbacks++;
}

void onBatteryLoggerCreated(
    void* context,
    MblMwDataLogger* logger
) {
    auto* state =
        static_cast<LoggerCreateState*>(
            context
        );

    if (state == nullptr) {
        return;
    }

    if (logger == nullptr) {
        if (state->output != nullptr) {
            state->output->error(
                "battery_logger_create_failed",
                "Battery logger creation failed"
            );
        }

        state->failures++;
    }
    else {
        if (state->output != nullptr) {
            state->output->event(
                "logger_created",
                {
                    {"sensor", "battery"}
                }
            );
        }

        state->battery_logger = logger;
    }

    state->callbacks++;
}

void onBatteryTimerCreated(
    void* context,
    MblMwTimer* timer
) {
    auto* state =
        static_cast<TimerCreateState*>(
            context
        );

    if (state == nullptr) {
        return;
    }

    if (timer == nullptr) {
        if (state->output != nullptr) {
            state->output->warning(
                "battery_timer_create_failed",
                "Battery timer creation attempt failed"
            );
        }

        state->failed.store(true);
    }
    else {
        if (state->output != nullptr) {
            state->output->event(
                "battery_timer_created"
            );
        }

        state->timer = timer;
    }

    state->done.store(true);
}

void onBatteryTimerCommandsRecorded(
    void* context,
    MblMwEvent* event,
    std::int32_t status
) {
    (void)event;

    auto* state =
        static_cast<EventRecordState*>(
            context
        );

    if (state == nullptr) {
        return;
    }

    if (status != 0) {
        if (state->output != nullptr) {
            state->output->event(
                "battery_timer_command_recording",
                {
                    {"success", false},
                    {"status", status}
                }
            );
        }

        state->failed.store(true);
    }
    else if (state->output != nullptr) {
        state->output->event(
            "battery_timer_command_recording",
            {
                {"success", true},
                {"status", status}
            }
        );
    }

    state->done.store(true);
}

bool tryCreateTimerIndefinite(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    std::uint32_t period_ms,
    MblMwTimer*& out_timer,
    CommandOutput& output
) {
    TimerCreateState state;
    state.output = &output;

    output.event(
        "battery_timer_attempt",
        {
            {"api", "indefinite"},
            {"period_ms", period_ms}
        }
    );

    mbl_mw_timer_create_indefinite(
        board,
        period_ms,
        0,
        &state,
        onBatteryTimerCreated
    );

    const bool ok =
        waitForDone(
            bridge,
            state,
            5000
        ) &&
        state.timer != nullptr;

    if (ok) {
        out_timer = state.timer;
        return true;
    }

    out_timer = nullptr;
    return false;
}

bool tryCreateTimerLegacy(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    std::uint32_t period_ms,
    MblMwTimer*& out_timer,
    CommandOutput& output
) {
    TimerCreateState state;
    state.output = &output;

    output.event(
        "battery_timer_attempt",
        {
            {"api", "legacy"},
            {"period_ms", period_ms}
        }
    );

    mbl_mw_timer_create(
        board,
        period_ms,
        0,
        0,
        &state,
        onBatteryTimerCreated
    );

    const bool ok =
        waitForDone(
            bridge,
            state,
            5000
        ) &&
        state.timer != nullptr;

    if (ok) {
        out_timer = state.timer;
        return true;
    }

    out_timer = nullptr;
    return false;
}

bool createBatteryTimerWithFallback(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    std::uint32_t battery_interval_ms,
    TimerCreateState& battery_timer_state,
    CommandOutput& output
) {
    output.status(
        "creating_battery_timer"
    );

    MblMwTimer* timer = nullptr;

    if (
        tryCreateTimerIndefinite(
            bridge,
            board,
            battery_interval_ms,
            timer,
            output
        )
    ) {
        battery_timer_state.output = &output;
        battery_timer_state.timer = timer;
        battery_timer_state.done.store(true);
        battery_timer_state.failed.store(false);
        return true;
    }

    output.warning(
        "battery_timer_indefinite_failed",
        "Indefinite timer creation failed; trying legacy timer API"
    );

    if (
        tryCreateTimerLegacy(
            bridge,
            board,
            battery_interval_ms,
            timer,
            output
        )
    ) {
        battery_timer_state.output = &output;
        battery_timer_state.timer = timer;
        battery_timer_state.done.store(true);
        battery_timer_state.failed.store(false);
        return true;
    }

    battery_timer_state.output = &output;
    battery_timer_state.timer = nullptr;
    battery_timer_state.done.store(true);
    battery_timer_state.failed.store(true);

    return false;
}

void runTimerDiagnostics(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    std::uint32_t requested_period_ms,
    CommandOutput& output
) {
    output.status(
        "timer_diagnostics"
    );

    emitCoreModuleInfo(
        output,
        board
    );

    const std::uint32_t periods[] = {
        requested_period_ms,
        1000,
        5000,
        60000
    };

    for (
        const std::uint32_t period_ms :
        periods
    ) {
        if (period_ms == 0) {
            continue;
        }

        MblMwTimer* timer = nullptr;

        if (
            tryCreateTimerIndefinite(
                bridge,
                board,
                period_ms,
                timer,
                output
            )
        ) {
            const std::uint8_t timer_id =
                mbl_mw_timer_get_id(
                    timer
                );

            output.event(
                "timer_diagnostic",
                {
                    {"period_ms", period_ms},
                    {"api", "indefinite"},
                    {"success", true},
                    {"timer_id", timer_id}
                }
            );

            mbl_mw_timer_remove(
                timer
            );

            pumpFor(
                bridge,
                250
            );

            continue;
        }

        if (
            tryCreateTimerLegacy(
                bridge,
                board,
                period_ms,
                timer,
                output
            )
        ) {
            const std::uint8_t timer_id =
                mbl_mw_timer_get_id(
                    timer
                );

            output.event(
                "timer_diagnostic",
                {
                    {"period_ms", period_ms},
                    {"api", "legacy"},
                    {"success", true},
                    {"timer_id", timer_id}
                }
            );

            mbl_mw_timer_remove(
                timer
            );

            pumpFor(
                bridge,
                250
            );

            continue;
        }

        output.event(
            "timer_diagnostic",
            {
                {"period_ms", period_ms},
                {"api", "both"},
                {"success", false}
            }
        );
    }
}

} // namespace

int runRecordStartCommand(
    const std::string& port_name,
    float sample_rate_hz,
    std::uint32_t battery_interval_seconds,
    CommandOutput& output
) {
    using namespace std::chrono_literals;

    validateSampleRate(
        sample_rate_hz
    );

    const bool battery_enabled =
        battery_interval_seconds > 0;

    output.started(
        {
            {"port", port_name},
            {"sample_rate_hz", sample_rate_hz},
            {
                "battery_enabled",
                battery_enabled
            },
            {
                "battery_interval_seconds",
                battery_interval_seconds
            }
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
        output.event(
            "sdk_init_result",
            {
                {
                    "status",
                    bridge.initializeStatus()
                }
            }
        );

        return fail(
            output,
            "sdk_init_failed",
            "SDK initialization failed",
            2
        );
    }

    auto* board =
        bridge.board();

    if (board == nullptr) {
        return fail(
            output,
            "null_board",
            "SDK returned a null board",
            2
        );
    }

    emitCoreModuleInfo(
        output,
        board
    );

    output.status(
        "configuring_accelerometer"
    );

    output.event(
        "sensor_config",
        {
            {"sensor", "accelerometer"},
            {"sample_rate_hz", sample_rate_hz},
            {"range_g", 4.0}
        }
    );

    mbl_mw_acc_set_odr(
        board,
        sample_rate_hz
    );

    mbl_mw_acc_set_range(
        board,
        4.0f
    );

    mbl_mw_acc_write_acceleration_config(
        board
    );

    pumpFor(
        bridge,
        250
    );

    const GyroImpl gyro_impl =
        detectGyroImpl(
            board,
            output
        );

    output.status(
        "configuring_gyro"
    );

    output.event(
        "sensor_config",
        {
            {"sensor", "gyroscope"},
            {"sample_rate_hz", sample_rate_hz},
            {"range_dps", 500}
        }
    );

    configureGyro(
        board,
        gyro_impl,
        sample_rate_hz
    );

    pumpFor(
        bridge,
        250
    );

    MblMwDataSignal* accel_signal =
        mbl_mw_acc_get_acceleration_data_signal(
            board
        );

    MblMwDataSignal* gyro_signal =
        getGyroSignal(
            board,
            gyro_impl
        );

    MblMwDataSignal* battery_signal =
        nullptr;

    if (battery_enabled) {
        battery_signal =
            mbl_mw_settings_get_battery_state_data_signal(
                board
            );
    }

    if (accel_signal == nullptr) {
        return fail(
            output,
            "accel_signal_missing",
            "Failed to get accelerometer data signal",
            3
        );
    }

    if (gyro_signal == nullptr) {
        return fail(
            output,
            "gyro_signal_missing",
            "Failed to get gyro data signal",
            3
        );
    }

    if (
        battery_enabled &&
        battery_signal == nullptr
    ) {
        return fail(
            output,
            "battery_signal_missing",
            "Failed to get battery data signal",
            3
        );
    }

    TimerCreateState battery_timer_state;
    battery_timer_state.output = &output;

    EventRecordState battery_event_state;
    battery_event_state.output = &output;

    if (battery_enabled) {
        const std::uint32_t battery_interval_ms =
            battery_interval_seconds *
            1000;

        if (
            !createBatteryTimerWithFallback(
                bridge,
                board,
                battery_interval_ms,
                battery_timer_state,
                output
            )
        ) {
            output.warning(
                "battery_timer_allocation_failed",
                "Battery timer allocation failed; running timer diagnostics"
            );

            runTimerDiagnostics(
                bridge,
                board,
                battery_interval_ms,
                output
            );

            return fail(
                output,
                "battery_timer_unavailable",
                "Battery logging cannot continue because no board timer could be allocated",
                6
            );
        }

        if (
            battery_timer_state.timer ==
            nullptr
        ) {
            return fail(
                output,
                "battery_timer_null",
                "Battery timer is null after creation",
                6
            );
        }

        const std::uint8_t battery_timer_id =
            mbl_mw_timer_get_id(
                battery_timer_state.timer
            );

        output.event(
            "battery_timer",
            {
                {
                    "timer_id",
                    battery_timer_id
                },
                {
                    "interval_seconds",
                    battery_interval_seconds
                }
            }
        );
    }

    LoggerCreateState logger_state;
    logger_state.output = &output;

    output.status(
        "creating_accelerometer_logger"
    );

    mbl_mw_datasignal_log(
        accel_signal,
        &logger_state,
        onAccelLoggerCreated
    );

    if (
        !waitForLoggerCallbacks(
            bridge,
            logger_state,
            1,
            5000
        )
    ) {
        return fail(
            output,
            "accel_logger_timeout",
            "Timed out waiting for accelerometer logger creation",
            4
        );
    }

    output.status(
        "creating_gyro_logger"
    );

    mbl_mw_datasignal_log(
        gyro_signal,
        &logger_state,
        onGyroLoggerCreated
    );

    if (
        !waitForLoggerCallbacks(
            bridge,
            logger_state,
            2,
            5000
        )
    ) {
        return fail(
            output,
            "gyro_logger_timeout",
            "Timed out waiting for gyro logger creation",
            4
        );
    }

    if (battery_enabled) {
        output.status(
            "creating_battery_logger"
        );

        mbl_mw_datasignal_log(
            battery_signal,
            &logger_state,
            onBatteryLoggerCreated
        );

        if (
            !waitForLoggerCallbacks(
                bridge,
                logger_state,
                3,
                5000
            )
        ) {
            return fail(
                output,
                "battery_logger_timeout",
                "Timed out waiting for battery logger creation",
                4
            );
        }
    }

    if (
        logger_state.failures.load() !=
        0
    ) {
        return fail(
            output,
            "logger_creation_failed",
            "One or more logger creations failed; run record-reset before starting a fresh session",
            5
        );
    }

    if (
        logger_state.accel_logger ==
        nullptr
    ) {
        return fail(
            output,
            "accel_logger_null",
            "Accelerometer logger is null after successful callback",
            5
        );
    }

    if (
        logger_state.gyro_logger ==
        nullptr
    ) {
        return fail(
            output,
            "gyro_logger_null",
            "Gyro logger is null after successful callback",
            5
        );
    }

    if (
        battery_enabled &&
        logger_state.battery_logger ==
        nullptr
    ) {
        return fail(
            output,
            "battery_logger_null",
            "Battery logger is null after successful callback",
            5
        );
    }

    const std::uint8_t accel_logger_id =
        mbl_mw_logger_get_id(
            logger_state.accel_logger
        );

    const std::uint8_t gyro_logger_id =
        mbl_mw_logger_get_id(
            logger_state.gyro_logger
        );

    output.event(
        "logger",
        {
            {"sensor", "acceleration"},
            {"logger_id", accel_logger_id}
        }
    );

    output.event(
        "logger",
        {
            {"sensor", "gyroscope"},
            {"logger_id", gyro_logger_id}
        }
    );

    if (battery_enabled) {
        const std::uint8_t battery_logger_id =
            mbl_mw_logger_get_id(
                logger_state.battery_logger
            );

        output.event(
            "logger",
            {
                {"sensor", "battery"},
                {
                    "logger_id",
                    battery_logger_id
                }
            }
        );
    }

    if (battery_enabled) {
        output.status(
            "recording_battery_timer_command"
        );

        auto* battery_event =
            reinterpret_cast<MblMwEvent*>(
                battery_timer_state.timer
            );

        mbl_mw_event_record_commands(
            battery_event
        );

        mbl_mw_datasignal_read(
            battery_signal
        );

        mbl_mw_event_end_record(
            battery_event,
            &battery_event_state,
            onBatteryTimerCommandsRecorded
        );

        if (
            !waitForDone(
                bridge,
                battery_event_state,
                5000
            )
        ) {
            return fail(
                output,
                "battery_timer_command_timeout",
                "Timed out waiting for battery timer command recording",
                7
            );
        }
    }

    output.status(
        "starting_internal_logging"
    );

    mbl_mw_logging_start(
        board,
        0
    );

    pumpFor(
        bridge,
        250
    );

    output.status(
        "starting_accelerometer"
    );

    mbl_mw_acc_enable_acceleration_sampling(
        board
    );

    mbl_mw_acc_start(
        board
    );

    pumpFor(
        bridge,
        250
    );

    output.status(
        "starting_gyro"
    );

    startGyro(
        board,
        gyro_impl
    );

    pumpFor(
        bridge,
        250
    );

    if (battery_enabled) {
        output.status(
            "starting_battery_timer"
        );

        mbl_mw_timer_start(
            battery_timer_state.timer
        );

        pumpFor(
            bridge,
            250
        );
    }

    output.event(
        "summary",
        {
            {
                "sample_rate_hz",
                sample_rate_hz
            },
            {
                "battery_enabled",
                battery_enabled
            },
            {
                "battery_interval_seconds",
                battery_interval_seconds
            },
            {
                "host_state_persisted",
                false
            },
            {
                "recording",
                true
            }
        }
    );

    output.completed(
        true,
        0
    );

    return 0;
}

int runRecordStartCommand(
    const std::string& port_name,
    float sample_rate_hz,
    std::uint32_t battery_interval_seconds
) {
    CommandOutput output(
        "record-start"
    );

    return runRecordStartCommand(
        port_name,
        sample_rate_hz,
        battery_interval_seconds,
        output
    );
}

} // namespace headmotion::app
