/*
 * RecordStartCommand
 * ------------------
 *
 * Starts an internal recording session on the MetaMotionS/MMS+ sensor using
 * the USB serial transport instead of BLE.
 *
 * Diagnostic focus:
 *
 *   - Print core module implementations, especially TIMER.
 *   - Create the battery timer before logger allocation.
 *   - If timer creation fails, probe several timer periods and both timer APIs.
 *   - Keep battery command recording active once a timer is successfully created.
 */

#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
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

enum class GyroImpl {
    Unknown,
    Bmi160,
    Bmi270
};

struct LoggerCreateState {
    std::atomic<int> callbacks{0};
    std::atomic<int> failures{0};

    MblMwDataLogger* accel_logger = nullptr;
    MblMwDataLogger* gyro_logger = nullptr;
    MblMwDataLogger* battery_logger = nullptr;
};

struct TimerCreateState {
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};

    MblMwTimer* timer = nullptr;
};

struct EventRecordState {
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
};

std::filesystem::path loggerMetadataPath() {
    return std::filesystem::path(
        headmotion::session::BoardStateStore::defaultPath() + ".loggers"
    );
}

void pumpFor(headmotion::sdk::MetaWearSdkBridge& bridge, int total_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

template <typename State>
bool waitForDone(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    State& state,
    int timeout_ms
) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(100);

        if (state.done.load()) {
            return !state.failed.load();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

void printModuleImpl(
    MblMwMetaWearBoard* board,
    MblMwModule module,
    const char* name
) {
    const int32_t impl =
        mbl_mw_metawearboard_lookup_module(board, module);

    std::cout << "  " << name << ": " << impl << "\n";
}

void printCoreModuleInfo(MblMwMetaWearBoard* board) {
    std::cout << "Core module implementations:\n";

    printModuleImpl(board, MBL_MW_MODULE_EVENT, "EVENT");
    printModuleImpl(board, MBL_MW_MODULE_LOGGING, "LOGGING");
    printModuleImpl(board, MBL_MW_MODULE_TIMER, "TIMER");
    printModuleImpl(board, MBL_MW_MODULE_SETTINGS, "SETTINGS");
    printModuleImpl(board, MBL_MW_MODULE_GYRO, "GYRO");
}

void saveLoggerMetadata(
    std::uint8_t accel_logger_id,
    std::uint8_t gyro_logger_id,
    bool battery_enabled,
    std::uint8_t battery_logger_id,
    std::uint8_t battery_timer_id,
    std::uint32_t battery_interval_seconds
) {
    const auto path = loggerMetadataPath();

    std::ofstream out(path);

    if (!out) {
        throw std::runtime_error(
            "Failed to open logger metadata file: " + path.string()
        );
    }

    out << "accel_logger_id=" << static_cast<int>(accel_logger_id) << "\n";
    out << "gyro_logger_id=" << static_cast<int>(gyro_logger_id) << "\n";
    out << "battery_enabled=" << (battery_enabled ? 1 : 0) << "\n";

    if (battery_enabled) {
        out << "battery_logger_id=" << static_cast<int>(battery_logger_id) << "\n";
        out << "battery_timer_id=" << static_cast<int>(battery_timer_id) << "\n";
        out << "battery_interval_seconds=" << battery_interval_seconds << "\n";
    }
}

void validateSampleRate(float sample_rate_hz) {
    if (sample_rate_hz != 25.0f &&
        sample_rate_hz != 50.0f &&
        sample_rate_hz != 100.0f &&
        sample_rate_hz != 200.0f &&
        sample_rate_hz != 400.0f &&
        sample_rate_hz != 800.0f &&
        sample_rate_hz != 1600.0f &&
        sample_rate_hz != 3200.0f) {
        throw std::runtime_error(
            "Unsupported sample rate. Use one of: 25, 50, 100, 200, 400, 800, 1600, 3200 Hz"
        );
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

    if (sample_rate_hz == 200.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_200Hz;
    }

    if (sample_rate_hz == 400.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_400Hz;
    }

    if (sample_rate_hz == 800.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_800Hz;
    }

    if (sample_rate_hz == 1600.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_1600Hz;
    }

    if (sample_rate_hz == 3200.0f) {
        return MBL_MW_GYRO_BOSCH_ODR_3200Hz;
    }

    throw std::runtime_error(
        "Unsupported sample rate. Use one of: 25, 50, 100, 200, 400, 800, 1600, 3200 Hz"
    );
}

void removeStaleBoardState() {
    const auto state_path = headmotion::session::BoardStateStore::defaultPath();

    if (std::filesystem::exists(state_path)) {
        std::filesystem::remove(state_path);
        std::cout << "Removed stale board state: " << state_path << "\n";
    }

    const auto metadata_path = loggerMetadataPath();

    if (std::filesystem::exists(metadata_path)) {
        std::filesystem::remove(metadata_path);
        std::cout << "Removed stale logger metadata: " << metadata_path << "\n";
    }
}

GyroImpl detectGyroImpl(MblMwMetaWearBoard* board) {
    if (board == nullptr) {
        throw std::runtime_error("Cannot detect gyro implementation: board is null");
    }

    const int32_t impl =
        mbl_mw_metawearboard_lookup_module(board, MBL_MW_MODULE_GYRO);

    std::cout << "Raw gyro implementation value: " << impl << "\n";

    if (impl < 0) {
        throw std::runtime_error("Gyroscope module is not present on this board");
    }

    if (impl == GYRO_IMPL_BMI160) {
        std::cout << "Gyro implementation: BMI160\n";
        return GyroImpl::Bmi160;
    }

    if (impl == GYRO_IMPL_BMI270) {
        std::cout << "Gyro implementation: BMI270\n";
        return GyroImpl::Bmi270;
    }

    throw std::runtime_error(
        "Unknown gyroscope implementation: " + std::to_string(impl)
    );
}

void configureGyro(
    MblMwMetaWearBoard* board,
    GyroImpl gyro_impl,
    float sample_rate_hz
) {
    const MblMwGyroBoschOdr odr = gyroOdrFromRate(sample_rate_hz);

    if (gyro_impl == GyroImpl::Bmi160) {
        mbl_mw_gyro_bmi160_set_odr(board, odr);
        mbl_mw_gyro_bmi160_set_range(board, MBL_MW_GYRO_BOSCH_RANGE_500dps);
        mbl_mw_gyro_bmi160_write_config(board);
        return;
    }

    if (gyro_impl == GyroImpl::Bmi270) {
        mbl_mw_gyro_bmi270_set_odr(board, odr);
        mbl_mw_gyro_bmi270_set_range(board, MBL_MW_GYRO_BOSCH_RANGE_500dps);
        mbl_mw_gyro_bmi270_write_config(board);
        return;
    }

    throw std::runtime_error("Cannot configure gyro: unknown implementation");
}

MblMwDataSignal* getGyroSignal(
    MblMwMetaWearBoard* board,
    GyroImpl gyro_impl
) {
    if (gyro_impl == GyroImpl::Bmi160) {
        return mbl_mw_gyro_bmi160_get_rotation_data_signal(board);
    }

    if (gyro_impl == GyroImpl::Bmi270) {
        return mbl_mw_gyro_bmi270_get_rotation_data_signal(board);
    }

    return nullptr;
}

void startGyro(
    MblMwMetaWearBoard* board,
    GyroImpl gyro_impl
) {
    if (gyro_impl == GyroImpl::Bmi160) {
        mbl_mw_gyro_bmi160_enable_rotation_sampling(board);
        mbl_mw_gyro_bmi160_start(board);
        return;
    }

    if (gyro_impl == GyroImpl::Bmi270) {
        mbl_mw_gyro_bmi270_enable_rotation_sampling(board);
        mbl_mw_gyro_bmi270_start(board);
        return;
    }

    throw std::runtime_error("Cannot start gyro: unknown implementation");
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

void onBatteryLoggerCreated(void* context, MblMwDataLogger* logger) {
    auto* state = static_cast<LoggerCreateState*>(context);

    if (logger == nullptr) {
        std::cout << "Battery logger creation failed\n";
        state->failures++;
    } else {
        std::cout << "Battery logger created\n";
        state->battery_logger = logger;
    }

    state->callbacks++;
}

void onBatteryTimerCreated(void* context, MblMwTimer* timer) {
    auto* state = static_cast<TimerCreateState*>(context);

    if (state == nullptr) {
        return;
    }

    if (timer == nullptr) {
        std::cout << "Battery timer creation failed\n";
        state->failed.store(true);
    } else {
        std::cout << "Battery timer created\n";
        state->timer = timer;
    }

    state->done.store(true);
}

void onBatteryTimerCommandsRecorded(
    void* context,
    MblMwEvent* event,
    int32_t status
) {
    (void)event;

    auto* state = static_cast<EventRecordState*>(context);

    if (state == nullptr) {
        return;
    }

    if (status != 0) {
        std::cout << "Battery timer command recording failed, status="
                  << status
                  << "\n";
        state->failed.store(true);
    } else {
        std::cout << "Battery timer command recording complete\n";
    }

    state->done.store(true);
}

bool tryCreateTimerIndefinite(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    std::uint32_t period_ms,
    MblMwTimer*& out_timer
) {
    TimerCreateState state;

    std::cout << "  trying mbl_mw_timer_create_indefinite period="
              << period_ms
              << " ms\n";

    mbl_mw_timer_create_indefinite(
        board,
        period_ms,
        0,
        &state,
        onBatteryTimerCreated
    );

    const bool ok =
        waitForDone(bridge, state, 5000) &&
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
    MblMwTimer*& out_timer
) {
    TimerCreateState state;

    std::cout << "  trying mbl_mw_timer_create repetitions=0 period="
              << period_ms
              << " ms\n";

    mbl_mw_timer_create(
        board,
        period_ms,
        0,
        0,
        &state,
        onBatteryTimerCreated
    );

    const bool ok =
        waitForDone(bridge, state, 5000) &&
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
    TimerCreateState& battery_timer_state
) {
    std::cout << "Creating battery timer early: "
              << battery_interval_ms
              << " ms\n";

    MblMwTimer* timer = nullptr;

    if (tryCreateTimerIndefinite(bridge, board, battery_interval_ms, timer)) {
        battery_timer_state.timer = timer;
        battery_timer_state.done.store(true);
        battery_timer_state.failed.store(false);
        return true;
    }

    std::cout
        << "Indefinite battery timer creation failed; "
        << "trying legacy repetitions=0 timer API\n";

    if (tryCreateTimerLegacy(bridge, board, battery_interval_ms, timer)) {
        battery_timer_state.timer = timer;
        battery_timer_state.done.store(true);
        battery_timer_state.failed.store(false);
        return true;
    }

    battery_timer_state.timer = nullptr;
    battery_timer_state.done.store(true);
    battery_timer_state.failed.store(true);
    return false;
}

void runTimerDiagnostics(
    headmotion::sdk::MetaWearSdkBridge& bridge,
    MblMwMetaWearBoard* board,
    std::uint32_t requested_period_ms
) {
    std::cout << "\nTimer diagnostics:\n";
    printCoreModuleInfo(board);

    const std::uint32_t periods[] = {
        requested_period_ms,
        1000,
        5000,
        60000
    };

    for (std::uint32_t period_ms : periods) {
        if (period_ms == 0) {
            continue;
        }

        std::cout << "Probing timer period: "
                  << period_ms
                  << " ms\n";

        MblMwTimer* timer = nullptr;

        if (tryCreateTimerIndefinite(bridge, board, period_ms, timer)) {
            const std::uint8_t timer_id = mbl_mw_timer_get_id(timer);

            std::cout << "  SUCCESS indefinite timer, id="
                      << static_cast<int>(timer_id)
                      << "\n";

            std::cout << "  removing diagnostic timer\n";
            mbl_mw_timer_remove(timer);
            pumpFor(bridge, 250);
            continue;
        }

        if (tryCreateTimerLegacy(bridge, board, period_ms, timer)) {
            const std::uint8_t timer_id = mbl_mw_timer_get_id(timer);

            std::cout << "  SUCCESS legacy timer, id="
                      << static_cast<int>(timer_id)
                      << "\n";

            std::cout << "  removing diagnostic timer\n";
            mbl_mw_timer_remove(timer);
            pumpFor(bridge, 250);
            continue;
        }

        std::cout << "  FAILED both timer APIs for period "
                  << period_ms
                  << " ms\n";
    }

    std::cout << "End timer diagnostics.\n\n";
}

} // namespace

int runRecordStartCommand(
    const std::string& port_name,
    float sample_rate_hz,
    std::uint32_t battery_interval_seconds
) {
    using namespace std::chrono_literals;

    validateSampleRate(sample_rate_hz);

    const bool battery_enabled = battery_interval_seconds > 0;

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

    printCoreModuleInfo(board);

    std::cout << "Configuring accelerometer: "
              << sample_rate_hz
              << " Hz, +/-4 g\n";

    mbl_mw_acc_set_odr(board, sample_rate_hz);
    mbl_mw_acc_set_range(board, 4.0f);
    mbl_mw_acc_write_acceleration_config(board);
    pumpFor(bridge, 250);

    const GyroImpl gyro_impl = detectGyroImpl(board);

    std::cout << "Configuring gyro: "
              << sample_rate_hz
              << " Hz, +/-500 dps\n";

    configureGyro(board, gyro_impl, sample_rate_hz);
    pumpFor(bridge, 250);

    MblMwDataSignal* accel_signal =
        mbl_mw_acc_get_acceleration_data_signal(board);

    MblMwDataSignal* gyro_signal =
        getGyroSignal(board, gyro_impl);

    MblMwDataSignal* battery_signal = nullptr;

    if (battery_enabled) {
        battery_signal =
            mbl_mw_settings_get_battery_state_data_signal(board);
    }

    if (accel_signal == nullptr) {
        std::cerr << "Failed to get accelerometer data signal\n";
        return 3;
    }

    if (gyro_signal == nullptr) {
        std::cerr << "Failed to get gyro data signal\n";
        return 3;
    }

    if (battery_enabled && battery_signal == nullptr) {
        std::cerr << "Failed to get battery data signal\n";
        return 3;
    }

    std::uint8_t battery_timer_id = 0xff;
    TimerCreateState battery_timer_state;
    EventRecordState battery_event_state;

    if (battery_enabled) {
        const std::uint32_t battery_interval_ms =
            battery_interval_seconds * 1000;

        if (!createBatteryTimerWithFallback(
                bridge,
                board,
                battery_interval_ms,
                battery_timer_state
            )) {
            std::cerr
                << "Battery timer creation failed before logger allocation.\n";

            runTimerDiagnostics(
                bridge,
                board,
                battery_interval_ms
            );

            std::cerr
                << "Battery logging cannot continue because no board timer "
                << "could be allocated.\n";

            return 6;
        }

        if (battery_timer_state.timer == nullptr) {
            std::cerr << "Battery timer is null after creation\n";
            return 6;
        }

        battery_timer_id = mbl_mw_timer_get_id(battery_timer_state.timer);

        std::cout << "Battery timer ID: "
                  << static_cast<int>(battery_timer_id)
                  << "\n";
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

    if (battery_enabled) {
        std::cout << "Creating battery logger\n";
        mbl_mw_datasignal_log(
            battery_signal,
            &logger_state,
            onBatteryLoggerCreated
        );

        if (!waitForLoggerCallbacks(bridge, logger_state, 3, 5000)) {
            std::cerr << "Timed out waiting for battery logger creation\n";
            return 4;
        }
    }

    if (logger_state.failures.load() != 0) {
        std::cerr << "One or more logger creations failed\n";
        std::cerr << "The board probably still has old logger routes allocated.\n";
        std::cerr << "Run record-reset before starting a fresh session.\n";
        return 5;
    }

    if (logger_state.accel_logger == nullptr) {
        std::cerr << "Accelerometer logger is null after successful callback\n";
        return 5;
    }

    if (logger_state.gyro_logger == nullptr) {
        std::cerr << "Gyro logger is null after successful callback\n";
        return 5;
    }

    if (battery_enabled && logger_state.battery_logger == nullptr) {
        std::cerr << "Battery logger is null after successful callback\n";
        return 5;
    }

    const std::uint8_t accel_logger_id =
        mbl_mw_logger_get_id(logger_state.accel_logger);

    const std::uint8_t gyro_logger_id =
        mbl_mw_logger_get_id(logger_state.gyro_logger);

    std::uint8_t battery_logger_id = 0xff;

    if (battery_enabled) {
        battery_logger_id =
            mbl_mw_logger_get_id(logger_state.battery_logger);
    }

    std::cout << "Accel logger ID: "
              << static_cast<int>(accel_logger_id)
              << "\n";

    std::cout << "Gyro logger ID: "
              << static_cast<int>(gyro_logger_id)
              << "\n";

    if (battery_enabled) {
        std::cout << "Battery logger ID: "
                  << static_cast<int>(battery_logger_id)
                  << "\n";
    }

    if (battery_enabled) {
        std::cout << "Recording battery read command onto timer\n";

        auto* battery_event =
            reinterpret_cast<MblMwEvent*>(battery_timer_state.timer);

        mbl_mw_event_record_commands(battery_event);

        mbl_mw_datasignal_read(battery_signal);

        mbl_mw_event_end_record(
            battery_event,
            &battery_event_state,
            onBatteryTimerCommandsRecorded
        );

        if (!waitForDone(bridge, battery_event_state, 5000)) {
            std::cerr << "Timed out waiting for battery timer command recording\n";
            return 7;
        }
    }

    std::cout << "Starting internal logging, overwrite=false\n";
    mbl_mw_logging_start(board, 0);
    pumpFor(bridge, 250);

    std::cout << "Starting accelerometer sampling\n";
    mbl_mw_acc_enable_acceleration_sampling(board);
    mbl_mw_acc_start(board);
    pumpFor(bridge, 250);

    std::cout << "Starting gyro sampling\n";
    startGyro(board, gyro_impl);
    pumpFor(bridge, 250);

    if (battery_enabled) {
        std::cout << "Starting battery timer\n";
        mbl_mw_timer_start(battery_timer_state.timer);
        pumpFor(bridge, 250);
    }

    saveLoggerMetadata(
        accel_logger_id,
        gyro_logger_id,
        battery_enabled,
        battery_logger_id,
        battery_timer_id,
        battery_interval_seconds
    );

    std::cout << "Saved logger metadata: "
              << loggerMetadataPath()
              << "\n";

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

    if (battery_enabled) {
        std::cout << "Battery logging enabled: every "
                  << battery_interval_seconds
                  << " seconds\n";
    } else {
        std::cout << "Battery logging disabled\n";
    }

    std::cout << "The MMS should now be internally logging accel + gyro";

    if (battery_enabled) {
        std::cout << " + battery";
    }

    std::cout << ".\n";
    std::cout << "Use record-stop before sync/download.\n";

    return 0;
}

} // namespace headmotion::app