/*
 * Developer Notes: SyncCommand
 * ----------------------------
 *
 * SyncCommand drains MetaWear onboard flash logs over the project's USB-backed
 * SDK bridge and writes decoded records to disk.
 *
 * This file intentionally does not configure sensors or create routes. That
 * work belongs to RecordStartCommand. SyncCommand assumes record-start has
 * already created the logger routes, serialized the SDK board state, and saved
 * logger metadata containing the logger IDs.
 */

#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/data.h"
#include "metawear/core/logging.h"
#include "metawear/core/types.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace headmotion::app {

namespace {

/*
 * During battery/debug runs, clearing after a successful sync prevents old log
 * entries from being downloaded again and mixed into the next CSV.
 *
 * Set this to false if you want sync to be non-destructive.
 */
constexpr bool CLEAR_AFTER_SUCCESSFUL_SYNC = true;

struct LoggerMetadata {
    int accel_logger_id = -1;
    int gyro_logger_id = -1;

    bool battery_enabled = false;
    int battery_logger_id = -1;
    int battery_timer_id = -1;
    std::uint32_t battery_interval_seconds = 0;
};

struct SyncState {
    std::ofstream imu_csv;
    std::ofstream battery_csv;
    std::mutex csv_mutex;

    std::atomic<bool> download_started{false};
    std::atomic<bool> download_done{false};

    std::atomic<std::uint32_t> entries_left{0};
    std::atomic<std::uint32_t> total_entries{0};

    std::atomic<std::uint64_t> imu_rows_written{0};
    std::atomic<std::uint64_t> battery_rows_written{0};
    std::atomic<std::uint64_t> unknown_entries{0};
    std::atomic<std::uint64_t> unhandled_entries{0};
};

std::filesystem::path loggerMetadataPath() {
    return std::filesystem::path(
        headmotion::session::BoardStateStore::defaultPath() + ".loggers"
    );
}

LoggerMetadata loadLoggerMetadata() {
    const auto path = loggerMetadataPath();

    std::ifstream in(path);

    if (!in) {
        throw std::runtime_error(
            "Failed to open logger metadata file: " + path.string() +
            ". Run record-start again so logger metadata is saved."
        );
    }

    LoggerMetadata metadata;

    std::string line;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');

        if (pos == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);

        if (key == "accel_logger_id") {
            metadata.accel_logger_id = std::stoi(value);
        } else if (key == "gyro_logger_id") {
            metadata.gyro_logger_id = std::stoi(value);
        } else if (key == "battery_enabled") {
            metadata.battery_enabled = std::stoi(value) != 0;
        } else if (key == "battery_logger_id") {
            metadata.battery_logger_id = std::stoi(value);
        } else if (key == "battery_timer_id") {
            metadata.battery_timer_id = std::stoi(value);
        } else if (key == "battery_interval_seconds") {
            metadata.battery_interval_seconds =
                static_cast<std::uint32_t>(std::stoul(value));
        }
    }

    if (metadata.accel_logger_id < 0) {
        throw std::runtime_error("Logger metadata missing accel_logger_id");
    }

    if (metadata.gyro_logger_id < 0) {
        throw std::runtime_error("Logger metadata missing gyro_logger_id");
    }

    if (metadata.battery_enabled && metadata.battery_logger_id < 0) {
        throw std::runtime_error(
            "Logger metadata says battery is enabled, but battery_logger_id is missing"
        );
    }

    return metadata;
}

void pumpFor(headmotion::sdk::MetaWearSdkBridge& bridge, int total_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

float readFloatLe(const std::uint8_t* bytes) {
    float value = 0.0f;
    std::memcpy(&value, bytes, sizeof(float));
    return value;
}

void writeImuDataRow(
    SyncState* state,
    const char* sensor,
    const MblMwData* data
) {
    if (state == nullptr || data == nullptr) {
        return;
    }

    if (data->value == nullptr || data->length != 12) {
        return;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data->value);

    const float x = readFloatLe(bytes + 0);
    const float y = readFloatLe(bytes + 4);
    const float z = readFloatLe(bytes + 8);

    std::lock_guard<std::mutex> lock(state->csv_mutex);

    state->imu_csv
        << data->epoch << ","
        << sensor << ","
        << x << ","
        << y << ","
        << z
        << "\n";

    state->imu_rows_written++;
}

void writeBatteryDataRow(
    SyncState* state,
    const MblMwData* data
) {
    if (state == nullptr || data == nullptr || data->value == nullptr) {
        return;
    }

    const auto* battery =
        static_cast<const MblMwBatteryState*>(data->value);

    std::lock_guard<std::mutex> lock(state->csv_mutex);

    state->battery_csv
        << data->epoch << ","
        << battery->voltage << ","
        << static_cast<int>(battery->charge)
        << "\n";

    state->battery_rows_written++;
}

void onAccelLoggerData(void* context, const MblMwData* data) {
    writeImuDataRow(static_cast<SyncState*>(context), "accel_g", data);
}

void onGyroLoggerData(void* context, const MblMwData* data) {
    writeImuDataRow(static_cast<SyncState*>(context), "gyro_dps", data);
}

void onBatteryLoggerData(void* context, const MblMwData* data) {
    writeBatteryDataRow(static_cast<SyncState*>(context), data);
}

void onProgressUpdate(
    void* context,
    std::uint32_t entries_left,
    std::uint32_t total_entries
) {
    auto* state = static_cast<SyncState*>(context);

    if (state == nullptr) {
        return;
    }

    state->download_started = true;
    state->entries_left = entries_left;
    state->total_entries = total_entries;

    if (entries_left == 0) {
        state->download_done = true;
    }
}

void onUnknownEntry(
    void* context,
    std::uint8_t id,
    std::int64_t epoch,
    const std::uint8_t* data,
    std::uint8_t length
) {
    (void)id;
    (void)epoch;
    (void)data;
    (void)length;

    auto* state = static_cast<SyncState*>(context);

    if (state != nullptr) {
        state->unknown_entries++;
    }
}

void onUnhandledEntry(void* context, const MblMwData* data) {
    (void)data;

    auto* state = static_cast<SyncState*>(context);

    if (state != nullptr) {
        state->unhandled_entries++;
    }
}

void closeCsvs(SyncState& state, bool battery_enabled) {
    if (state.imu_csv.is_open()) {
        state.imu_csv.flush();
        state.imu_csv.close();
    }

    if (battery_enabled && state.battery_csv.is_open()) {
        state.battery_csv.flush();
        state.battery_csv.close();
    }
}

std::uint64_t totalRowsWritten(const SyncState& state) {
    return state.imu_rows_written.load() +
           state.battery_rows_written.load();
}

} // namespace

int runSyncCommand(const std::string& port_name, const std::string& output_path) {
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

    const auto state_path = headmotion::session::BoardStateStore::defaultPath();

    std::cout << "Loading board state: " << state_path << "\n";
    const auto board_state =
        headmotion::session::BoardStateStore::load(state_path);

    std::cout << "Deserializing board state ["
              << board_state.size()
              << " bytes]\n";

    bridge.deserializeBoard(board_state);
    pumpFor(bridge, 250);

    std::cout << "Loading logger metadata: "
              << loggerMetadataPath()
              << "\n";

    const LoggerMetadata metadata = loadLoggerMetadata();

    std::cout << "Accel logger ID: "
              << metadata.accel_logger_id
              << "\n";

    std::cout << "Gyro logger ID: "
              << metadata.gyro_logger_id
              << "\n";

    if (metadata.battery_enabled) {
        std::cout << "Battery logger ID: "
                  << metadata.battery_logger_id
                  << "\n";

        if (metadata.battery_timer_id >= 0) {
            std::cout << "Battery timer ID: "
                      << metadata.battery_timer_id
                      << "\n";
        }

        std::cout << "Battery interval: "
                  << metadata.battery_interval_seconds
                  << " seconds\n";
    } else {
        std::cout << "Battery logging was disabled for this session\n";
    }

    const std::filesystem::path output_dir{output_path};
    std::filesystem::create_directories(output_dir);

    const auto imu_path = output_dir / "imu.csv";
    const auto battery_path = output_dir / "battery.csv";

    SyncState sync_state;

    sync_state.imu_csv.open(imu_path, std::ios::binary);

    if (!sync_state.imu_csv) {
        std::cerr << "Failed to open IMU CSV: " << imu_path << "\n";
        return 3;
    }

    sync_state.imu_csv
        << "epoch_ms,"
        << "sensor,"
        << "x,"
        << "y,"
        << "z"
        << "\n";

    if (metadata.battery_enabled) {
        sync_state.battery_csv.open(battery_path, std::ios::binary);

        if (!sync_state.battery_csv) {
            std::cerr << "Failed to open battery CSV: " << battery_path << "\n";
            sync_state.imu_csv.close();
            return 3;
        }

        sync_state.battery_csv
            << "epoch_ms,"
            << "voltage_mv,"
            << "charge_percent"
            << "\n";
    }

    auto* board = bridge.board();

    MblMwDataLogger* accel_logger =
        mbl_mw_logger_lookup_id(
            board,
            static_cast<std::uint8_t>(metadata.accel_logger_id)
        );

    MblMwDataLogger* gyro_logger =
        mbl_mw_logger_lookup_id(
            board,
            static_cast<std::uint8_t>(metadata.gyro_logger_id)
        );

    MblMwDataLogger* battery_logger = nullptr;

    if (metadata.battery_enabled) {
        battery_logger =
            mbl_mw_logger_lookup_id(
                board,
                static_cast<std::uint8_t>(metadata.battery_logger_id)
            );
    }

    if (accel_logger == nullptr) {
        std::cerr << "Could not look up accelerometer logger ID "
                  << metadata.accel_logger_id
                  << "\n";
        closeCsvs(sync_state, metadata.battery_enabled);
        return 4;
    }

    if (gyro_logger == nullptr) {
        std::cerr << "Could not look up gyro logger ID "
                  << metadata.gyro_logger_id
                  << "\n";
        closeCsvs(sync_state, metadata.battery_enabled);
        return 4;
    }

    if (metadata.battery_enabled && battery_logger == nullptr) {
        std::cerr << "Could not look up battery logger ID "
                  << metadata.battery_logger_id
                  << "\n";
        closeCsvs(sync_state, metadata.battery_enabled);
        return 4;
    }

    std::cout << "Subscribing accel logger\n";
    mbl_mw_logger_subscribe(
        accel_logger,
        &sync_state,
        onAccelLoggerData
    );

    std::cout << "Subscribing gyro logger\n";
    mbl_mw_logger_subscribe(
        gyro_logger,
        &sync_state,
        onGyroLoggerData
    );

    if (metadata.battery_enabled) {
        std::cout << "Subscribing battery logger\n";
        mbl_mw_logger_subscribe(
            battery_logger,
            &sync_state,
            onBatteryLoggerData
        );
    }

    MblMwLogDownloadHandler download_handler = {};
    download_handler.context = &sync_state;
    download_handler.received_progress_update = onProgressUpdate;
    download_handler.received_unknown_entry = onUnknownEntry;
    download_handler.received_unhandled_entry = onUnhandledEntry;

    std::cout << "Starting log download\n";
    mbl_mw_logging_download(board, 255, &download_handler);

    constexpr auto IDLE_TIMEOUT = std::chrono::minutes(2);

    auto last_progress_time = std::chrono::steady_clock::now();

    std::uint64_t last_rows_written =
        totalRowsWritten(sync_state);

    std::uint32_t last_entries_left =
        sync_state.entries_left.load();

    std::uint32_t last_total_entries =
        sync_state.total_entries.load();

    auto last_progress_print_time = std::chrono::steady_clock::now();
    bool printed_progress_line = false;

    while (!sync_state.download_done.load()) {
        bridge.pumpOnce(10);

        const auto now = std::chrono::steady_clock::now();

        const std::uint64_t current_rows_written =
            totalRowsWritten(sync_state);

        const std::uint32_t current_entries_left =
            sync_state.entries_left.load();

        const std::uint32_t current_total_entries =
            sync_state.total_entries.load();

        const bool rows_changed =
            current_rows_written != last_rows_written;

        const bool progress_changed =
            current_entries_left != last_entries_left ||
            current_total_entries != last_total_entries;

        const bool should_print_progress =
            sync_state.download_started.load() &&
            (
                now - last_progress_print_time >= std::chrono::seconds(1) ||
                current_entries_left == 0
            );

        if (should_print_progress) {
            const std::uint32_t entries_downloaded =
                current_total_entries >= current_entries_left
                    ? current_total_entries - current_entries_left
                    : 0;

            const std::uint32_t percent =
                current_total_entries > 0
                    ? static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(entries_downloaded) * 100) /
                        current_total_entries
                    )
                    : 0;

            std::cout
                << "\rDownload: "
                << percent
                << "%"
                << std::flush;

            printed_progress_line = true;
            last_progress_print_time = now;
        }

        if (rows_changed || progress_changed) {
            last_progress_time = now;

            last_rows_written = current_rows_written;
            last_entries_left = current_entries_left;
            last_total_entries = current_total_entries;
        }

        if (sync_state.download_started.load() &&
            now - last_progress_time > IDLE_TIMEOUT) {
            std::cerr << "\nSync timed out: no download progress for 2 minutes.\n";
            std::cerr << "IMU rows written so far: "
                      << sync_state.imu_rows_written.load()
                      << "\n";
            std::cerr << "Battery rows written so far: "
                      << sync_state.battery_rows_written.load()
                      << "\n";
            std::cerr << "Entries left: "
                      << sync_state.entries_left.load()
                      << " / "
                      << sync_state.total_entries.load()
                      << "\n";
            std::cerr << "Unknown entries: "
                      << sync_state.unknown_entries.load()
                      << "\n";
            std::cerr << "Unhandled entries: "
                      << sync_state.unhandled_entries.load()
                      << "\n";
            std::cerr << "Output directory: "
                      << output_dir
                      << "\n";

            closeCsvs(sync_state, metadata.battery_enabled);
            return 5;
        }

        if (!sync_state.download_started.load() &&
            now - last_progress_time > IDLE_TIMEOUT) {
            std::cerr << "\nSync timed out: download did not start within 2 minutes.\n";
            std::cerr << "IMU rows written so far: "
                      << sync_state.imu_rows_written.load()
                      << "\n";
            std::cerr << "Battery rows written so far: "
                      << sync_state.battery_rows_written.load()
                      << "\n";
            std::cerr << "Output directory: "
                      << output_dir
                      << "\n";

            closeCsvs(sync_state, metadata.battery_enabled);
            return 5;
        }

        std::this_thread::sleep_for(1ms);
    }

    closeCsvs(sync_state, metadata.battery_enabled);

    if (printed_progress_line) {
        std::cout << "\n";
    }

    if (CLEAR_AFTER_SUCCESSFUL_SYNC) {
        std::cout << "Clearing downloaded log entries\n";
        mbl_mw_logging_clear_entries(board);
        pumpFor(bridge, 2000);
    }

    std::cout << "Sync complete.\n";
    std::cout << "IMU rows written: "
              << sync_state.imu_rows_written.load()
              << "\n";

    if (metadata.battery_enabled) {
        std::cout << "Battery rows written: "
                  << sync_state.battery_rows_written.load()
                  << "\n";
    }

    std::cout << "Unknown entries: "
              << sync_state.unknown_entries.load()
              << "\n";

    std::cout << "Unhandled entries: "
              << sync_state.unhandled_entries.load()
              << "\n";

    std::cout << "IMU CSV: "
              << imu_path
              << "\n";

    if (metadata.battery_enabled) {
        std::cout << "Battery CSV: "
                  << battery_path
                  << "\n";
    }

    return 0;
}

} // namespace headmotion::app