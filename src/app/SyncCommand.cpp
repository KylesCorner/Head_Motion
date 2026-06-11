#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/util/Hex.hpp"

extern "C" {
#include "metawear/core/data.h"
#include "metawear/core/logging.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstring>

namespace headmotion::app {

namespace {

struct SyncState {
    std::ofstream csv;
    std::mutex csv_mutex;

    std::atomic<bool> download_started{false};
    std::atomic<bool> download_done{false};

    std::atomic<std::uint32_t> entries_left{0};
    std::atomic<std::uint32_t> total_entries{0};

    std::atomic<std::uint64_t> rows_written{0};
};

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

std::string dataTypeName(std::uint8_t type_id) {
    switch (type_id) {
        case 0x03:
            return "cartesian_float";
        case 0x04:
            return "uint32";
        case 0x05:
            return "float";
        default:
            return "type_" + std::to_string(type_id);
    }
}

std::string bytesAsHex(const MblMwData* data) {
    if (data == nullptr || data->value == nullptr || data->length == 0) {
        return "";
    }

    const auto* ptr = static_cast<const std::uint8_t*>(data->value);
    const std::vector<std::uint8_t> bytes(ptr, ptr + data->length);
    return headmotion::util::hexDump(bytes);
}

void writeDataRow(
    SyncState* state,
    int logger_id,
    const char* label,
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

    std::string sensor = label;

    /*
     * Based on your current output, logger 1 is the decoded accel vector.
     * We will tighten this mapping once we see the decoded gyro rows too.
     */
    if (logger_id == 0 || logger_id == 1) {
        sensor = "accel_g";
    } else if (logger_id == 2 || logger_id == 3) {
        sensor = "gyro_dps";
    }

    std::lock_guard<std::mutex> lock(state->csv_mutex);

    state->csv
        << data->epoch << ","
        << sensor << ","
        << x << ","
        << y << ","
        << z
        << "\n";

    state->rows_written++;
}

void onLogger0Data(void* context, const MblMwData* data) {
    writeDataRow(static_cast<SyncState*>(context), 0, "accel_part0", data);
}

void onLogger1Data(void* context, const MblMwData* data) {
    writeDataRow(static_cast<SyncState*>(context), 1, "accel_part1", data);
}

void onLogger2Data(void* context, const MblMwData* data) {
    writeDataRow(static_cast<SyncState*>(context), 2, "gyro_part0", data);
}

void onLogger3Data(void* context, const MblMwData* data) {
    writeDataRow(static_cast<SyncState*>(context), 3, "gyro_part1", data);
}

void onProgressUpdate(void* context, std::uint32_t entries_left, std::uint32_t total_entries) {
    auto* state = static_cast<SyncState*>(context);

    state->download_started = true;
    state->entries_left = entries_left;
    state->total_entries = total_entries;

    // std::cout << "Download progress: "
    //           << (total_entries - entries_left)
    //           << "/"
    //           << total_entries
    //           << " entries"
    //           << " left="
    //           << entries_left
    //           << "\n";

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
    (void)context;
    (void)id;
    (void)epoch;
    (void)data;
    (void)length;
}

void ensureParentDirectory(const std::string& output_path) {
    const std::filesystem::path path{output_path};

    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
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
    const auto board_state = headmotion::session::BoardStateStore::load(state_path);

    std::cout << "Deserializing board state [" << board_state.size() << " bytes]\n";
    bridge.deserializeBoard(board_state);
    pumpFor(bridge, 250);

    ensureParentDirectory(output_path);

    SyncState sync_state;
    sync_state.csv.open(output_path, std::ios::binary);

    if (!sync_state.csv) {
        std::cerr << "Failed to open output CSV: " << output_path << "\n";
        return 3;
    }
    sync_state.csv
        << "epoch_ms,"
        << "sensor,"
        << "x,"
        << "y,"
        << "z"
        << "\n";

    auto* board = bridge.board();

    std::cout << "Looking up logger IDs 0, 1, 2, 3\n";

    MblMwDataLogger* logger0 = mbl_mw_logger_lookup_id(board, 0);
    MblMwDataLogger* logger1 = mbl_mw_logger_lookup_id(board, 1);
    MblMwDataLogger* logger2 = mbl_mw_logger_lookup_id(board, 2);
    MblMwDataLogger* logger3 = mbl_mw_logger_lookup_id(board, 3);

    if (logger0 == nullptr) {
        std::cerr << "Could not look up logger ID 0\n";
    }
    if (logger1 == nullptr) {
        std::cerr << "Could not look up logger ID 1\n";
    }
    if (logger2 == nullptr) {
        std::cerr << "Could not look up logger ID 2\n";
    }
    if (logger3 == nullptr) {
        std::cerr << "Could not look up logger ID 3\n";
    }

    if (logger0 == nullptr && logger1 == nullptr && logger2 == nullptr && logger3 == nullptr) {
        std::cerr << "No known loggers were found in deserialized state.\n";
        std::cerr << "Run record-start again after board-state saving is enabled.\n";
        return 4;
    }

    if (logger0 != nullptr) {
        std::cout << "Subscribing logger 0\n";
        mbl_mw_logger_subscribe(logger0, &sync_state, onLogger0Data);
    }

    if (logger1 != nullptr) {
        std::cout << "Subscribing logger 1\n";
        mbl_mw_logger_subscribe(logger1, &sync_state, onLogger1Data);
    }

    if (logger2 != nullptr) {
        std::cout << "Subscribing logger 2\n";
        mbl_mw_logger_subscribe(logger2, &sync_state, onLogger2Data);
    }

    if (logger3 != nullptr) {
        std::cout << "Subscribing logger 3\n";
        mbl_mw_logger_subscribe(logger3, &sync_state, onLogger3Data);
    }

    MblMwLogDownloadHandler download_handler = {};
    download_handler.context = &sync_state;
    download_handler.received_progress_update = onProgressUpdate;
    download_handler.received_unknown_entry = onUnknownEntry;

    std::cout << "Starting log download\n";
    mbl_mw_logging_download(board, 255, &download_handler);

    const auto deadline = std::chrono::steady_clock::now() + 60s;

    while (std::chrono::steady_clock::now() < deadline) {
        bridge.pumpOnce(10);

        if (sync_state.download_done.load()) {
            break;
        }

        std::this_thread::sleep_for(1ms);
    }

    sync_state.csv.flush();
    sync_state.csv.close();

    if (!sync_state.download_done.load()) {
        std::cerr << "Sync timed out before download completed.\n";
        std::cerr << "Rows written so far: " << sync_state.rows_written.load() << "\n";
        std::cerr << "Output CSV: " << output_path << "\n";
        return 5;
    }

    std::cout << "Sync complete.\n";
    std::cout << "Rows written: " << sync_state.rows_written.load() << "\n";
    std::cout << "Output CSV: " << output_path << "\n";

    return 0;
}

} // namespace headmotion::app