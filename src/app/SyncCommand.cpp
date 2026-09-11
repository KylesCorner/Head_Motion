/*
 * Developer Notes: SyncCommand
 * ----------------------------
 *
 * SyncCommand drains MetaWear onboard flash logs over the project's USB-backed
 * SDK bridge and writes decoded records to disk.
 *
 * The sync path is intentionally stateless.  It performs a fresh SDK
 * initialization, asks the MMS+ which logger routes currently exist, subscribes
 * to those routes as anonymous data signals, and downloads the onboard flash.
 *
 * No serialized board state, saved logger IDs, or per-device host metadata are
 * required.
 */

#include "headmotion/app/Commands.hpp"
#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/anonymous_datasignal.h"
#include "metawear/core/data.h"
#include "metawear/core/metawearboard.h"
#include "metawear/core/logging.h"
#include "metawear/core/types.h"
}

#include <atomic>
#include <iomanip>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace headmotion::app {

    namespace {

        /*
         * During battery/debug runs, clearing after a successful sync prevents old log
         * entries from being downloaded again and mixed into the next CSV.
         *
         * Set this to false if you want sync to be non-destructive.
         */
        constexpr bool CLEAR_AFTER_SUCCESSFUL_SYNC = false;

        struct TimedVectorSample {
            std::int64_t epoch_ms = 0;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        struct SyncState {
            std::ofstream imu_csv;
            std::ofstream xsens_csv;
            std::ofstream battery_csv;
            std::ofstream xsens_accel_temp;
            std::ofstream xsens_gyro_temp;
            std::mutex csv_mutex;

            headmotion::app::SyncProgressCallback
                progress_callback;

            /*
             * Absolute timestamps come directly from MblMwData::epoch.
             * Elapsed time is relative to the first sample observed in this sync.
             */
            std::optional<std::int64_t> first_sdk_epoch_ms;

            /*
             * Anonymous logger discovery state.  Keep it here so the callback context
             * remains valid for the entire lifetime of runSyncCommand.
             */
            std::atomic<bool> anonymous_discovery_done{ false };
            std::atomic<std::int32_t> anonymous_discovery_status{ -999 };
            std::vector<MblMwAnonymousDataSignal*> anonymous_signals;

            std::atomic<bool> download_started{ false };
            std::atomic<bool> download_done{ false };

            std::atomic<std::uint32_t> entries_left{ 0 };
            std::atomic<std::uint32_t> total_entries{ 0 };

            std::atomic<std::uint64_t> imu_rows_written{ 0 };
            std::atomic<std::uint64_t> xsens_rows_written{ 0 };
            std::atomic<std::uint64_t> battery_rows_written{ 0 };
            std::uint64_t xsens_packet_counter = 0;
            std::uint64_t xsens_unmatched_accel = 0;
            std::uint64_t xsens_unmatched_gyro = 0;
            std::uint32_t xsens_pair_tolerance_ms = 20;
            std::atomic<bool> xsens_temp_write_failed{ false };
            std::atomic<std::uint64_t> unknown_entries{ 0 };
            std::atomic<std::uint64_t> unhandled_entries{ 0 };
        };

        struct CsvOutputPaths {
            std::filesystem::path imu;
            std::filesystem::path xsens;
            std::filesystem::path battery;
            std::filesystem::path xsens_accel_temp;
            std::filesystem::path xsens_gyro_temp;
        };

        CsvOutputPaths chooseUnusedCsvOutputPaths(
            const std::filesystem::path& output_dir
        ) {
            for (std::uint64_t index = 0; ; ++index) {
                const std::string suffix =
                    index == 0
                    ? std::string{}
                : "_" + std::to_string(index);

                CsvOutputPaths candidate{
                    output_dir / ("imu" + suffix + ".csv"),
                    output_dir / ("imu_xsens" + suffix + ".csv"),
                    output_dir / ("battery" + suffix + ".csv"),
                    output_dir / (".imu_xsens_accel" + suffix + ".bin"),
                    output_dir / (".imu_xsens_gyro" + suffix + ".bin")
                };

                /*
                 * Treat any related file as a collision so all outputs retain the
                 * same session number.
                 */
                if (!std::filesystem::exists(candidate.imu) &&
                    !std::filesystem::exists(candidate.xsens) &&
                    !std::filesystem::exists(candidate.battery) &&
                    !std::filesystem::exists(candidate.xsens_accel_temp) &&
                    !std::filesystem::exists(candidate.xsens_gyro_temp)) {
                    return candidate;
                }
            }
        }
        bool openCsvForAppend(
            std::ofstream& stream,
            const std::filesystem::path& path,
            const char* header
        ) {
            stream.open(
                path,
                std::ios::out |
                std::ios::binary |
                std::ios::app
            );

            if (!stream.is_open()) {
                std::cerr << "Failed to open CSV: " << path << "\n";
                return false;
            }

            /*
             * std::ios::app guarantees that writes go to the end of the file.
             * Only write a header when the file is empty.
             *
             * Normally chooseUnusedCsvOutputPaths() gives us a new file, but this
             * check provides an independent second safeguard.
             */
            std::error_code size_error;
            const std::uintmax_t size =
                std::filesystem::file_size(path, size_error);

            if (size_error) {
                std::cerr
                    << "Failed to inspect CSV size: "
                    << path
                    << ": "
                    << size_error.message()
                    << "\n";

                stream.close();
                return false;
            }

            if (size == 0) {
                stream << header;
                stream.flush();

                if (!stream) {
                    std::cerr
                        << "Failed to write CSV header: "
                        << path
                        << "\n";

                    stream.close();
                    return false;
                }
            }

            return true;
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
        const char* dataTypeName(MblMwDataTypeId type_id) {
            switch (type_id) {
            case MBL_MW_DT_ID_UINT32:
                return "UINT32";
            case MBL_MW_DT_ID_FLOAT:
                return "FLOAT";
            case MBL_MW_DT_ID_CARTESIAN_FLOAT:
                return "CARTESIAN_FLOAT";
            case MBL_MW_DT_ID_INT32:
                return "INT32";
            case MBL_MW_DT_ID_BYTE_ARRAY:
                return "BYTE_ARRAY";
            case MBL_MW_DT_ID_BATTERY_STATE:
                return "BATTERY_STATE";
            default:
                return "UNKNOWN";
            }
        }

        void printUnexpectedData(
            const char* stream_name,
            const MblMwData* data,
            const char* reason
        ) {
            if (data == nullptr) {
                std::cerr << stream_name << ": null MblMwData: " << reason << "\n";
                return;
            }

            std::cerr
                << stream_name
                << ": skipping unexpected data: "
                << reason
                << ", epoch="
                << data->epoch
                << ", type="
                << static_cast<int>(data->type_id)
                << " ("
                << dataTypeName(data->type_id)
                << ")"
                << ", length="
                << static_cast<int>(data->length)
                << ", value="
                << data->value
                << "\n";
        }
        struct RowTimestamp {
            std::int64_t epoch_ms = 0;
            std::int64_t elapsed_ms = 0;
            std::string utc_timestamp = "";
        };

        RowTimestamp resolveRowTimestampLocked(
            SyncState& state,
            std::int64_t sdk_epoch_ms
        ) {
            RowTimestamp timestamp;
            timestamp.epoch_ms = sdk_epoch_ms;

            if (!state.first_sdk_epoch_ms.has_value()) {
                state.first_sdk_epoch_ms = sdk_epoch_ms;
            }

            timestamp.elapsed_ms =
                sdk_epoch_ms -
                *state.first_sdk_epoch_ms;

            /*
             * MblMwData::epoch is Unix epoch milliseconds.  Format the same value as
             * an explicit ISO-8601 UTC timestamp.  Use the thread-safe platform API on
             * Windows because this client is also built and run there.
             */
            std::int64_t seconds = sdk_epoch_ms / 1000;
            std::int64_t milliseconds = sdk_epoch_ms % 1000;

            if (milliseconds < 0) {
                milliseconds += 1000;
                --seconds;
            }

            const std::time_t time_value =
                static_cast<std::time_t>(seconds);

            std::tm utc_tm{};

#ifdef _WIN32
            if (gmtime_s(&utc_tm, &time_value) != 0) {
                timestamp.utc_timestamp = "INVALID_UTC";
                return timestamp;
            }
#else
            if (gmtime_r(&time_value, &utc_tm) == nullptr) {
                timestamp.utc_timestamp = "INVALID_UTC";
                return timestamp;
            }
#endif

            std::ostringstream formatted;
            formatted
                << std::put_time(
                    &utc_tm,
                    "%Y-%m-%dT%H:%M:%S"
                )
                << "."
                << std::setw(3)
                << std::setfill('0')
                << milliseconds
                << "Z";

            timestamp.utc_timestamp =
                formatted.str();

            return timestamp;
        }

        void writeImuDataRow(
            SyncState* state,
            const char* sensor,
            const MblMwData* data
        ) {
            if (state == nullptr) {
                return;
            }

            if (data == nullptr) {
                printUnexpectedData(
                    sensor,
                    data,
                    "data is null"
                );

                return;
            }

            if (data->value == nullptr) {
                printUnexpectedData(
                    sensor,
                    data,
                    "data->value is null"
                );

                return;
            }

            if (data->type_id !=
                MBL_MW_DT_ID_CARTESIAN_FLOAT) {
                printUnexpectedData(
                    sensor,
                    data,
                    "expected CARTESIAN_FLOAT"
                );

                return;
            }

            if (data->length <
                sizeof(MblMwCartesianFloat)) {
                printUnexpectedData(
                    sensor,
                    data,
                    "value is shorter than MblMwCartesianFloat"
                );

                return;
            }

            const auto* value =
                static_cast<const MblMwCartesianFloat*>(
                    data->value
                    );

            std::lock_guard<std::mutex> lock(
                state->csv_mutex
            );

            if (!state->imu_csv.is_open()) {
                printUnexpectedData(
                    sensor,
                    data,
                    "imu_csv is not open"
                );

                return;
            }

            const RowTimestamp timestamp =
                resolveRowTimestampLocked(
                    *state,
                    data->epoch
                );

            state->imu_csv
                << timestamp.epoch_ms
                << ","
                << timestamp.elapsed_ms
                << ","
                << timestamp.utc_timestamp
                << ","
                << sensor
                << ","
                << value->x
                << ","
                << value->y
                << ","
                << value->z
                << "\n";

            if (!state->imu_csv) {
                std::cerr
                    << "Failed to write IMU CSV row\n";

                return;
            }

            TimedVectorSample sample;
            sample.epoch_ms = timestamp.epoch_ms;
            sample.x = value->x;
            sample.y = value->y;
            sample.z = value->z;

            std::ofstream* temp_stream = nullptr;

            if (std::strcmp(sensor, "accel_g") == 0) {
                temp_stream = &state->xsens_accel_temp;
            }
            else if (std::strcmp(sensor, "gyro_dps") == 0) {
                temp_stream = &state->xsens_gyro_temp;
            }

            if (temp_stream != nullptr) {
                temp_stream->write(
                    reinterpret_cast<const char*>(&sample),
                    sizeof(sample)
                );

                if (!*temp_stream) {
                    state->xsens_temp_write_failed = true;
                    std::cerr
                        << "Failed to write temporary Xsens pairing data for "
                        << sensor
                        << "\n";
                }
            }

            state->imu_rows_written++;
        }

        bool readTimedVectorSample(
            std::ifstream& stream,
            TimedVectorSample& sample
        ) {
            stream.read(
                reinterpret_cast<char*>(&sample),
                sizeof(sample)
            );

            if (stream.gcount() == 0 && stream.eof()) {
                return false;
            }

            if (stream.gcount() != static_cast<std::streamsize>(sizeof(sample))) {
                throw std::runtime_error(
                    "Truncated temporary Xsens pairing file"
                );
            }

            return true;
        }

        std::uint64_t timestampDifferenceMs(
            std::int64_t lhs,
            std::int64_t rhs
        ) {
            return lhs >= rhs
                ? static_cast<std::uint64_t>(lhs - rhs)
                : static_cast<std::uint64_t>(rhs - lhs);
        }

        bool writeXsensWideRow(
            SyncState& state,
            const TimedVectorSample& accel,
            const TimedVectorSample& gyro
        ) {
            const RowTimestamp timestamp =
                resolveRowTimestampLocked(
                    state,
                    gyro.epoch_ms
                );

            state.xsens_csv
                << state.xsens_packet_counter++
                << ","
                << timestamp.epoch_ms
                << ",0,0,0,"
                << accel.x
                << ","
                << accel.y
                << ","
                << accel.z
                << ","
                << gyro.x
                << ","
                << gyro.y
                << ","
                << gyro.z
                << ","
                << timestamp.elapsed_ms
                << ","
                << timestamp.utc_timestamp
                << "\n";

            if (!state.xsens_csv) {
                std::cerr
                    << "Failed to write Xsens-compatible CSV row\n";
                return false;
            }

            state.xsens_rows_written++;
            return true;
        }

        bool finalizeXsensCsv(
            SyncState& state,
            const CsvOutputPaths& paths
        ) {
            {
                std::lock_guard<std::mutex> lock(state.csv_mutex);

                state.xsens_accel_temp.flush();
                state.xsens_accel_temp.close();
                state.xsens_gyro_temp.flush();
                state.xsens_gyro_temp.close();

                if (state.xsens_temp_write_failed.load()) {
                    std::cerr
                        << "Cannot build Xsens-compatible CSV because temporary "
                        << "IMU pairing data could not be written.\n";
                    return false;
                }
            }

            std::ifstream accel_in(
                paths.xsens_accel_temp,
                std::ios::in | std::ios::binary
            );
            std::ifstream gyro_in(
                paths.xsens_gyro_temp,
                std::ios::in | std::ios::binary
            );

            if (!accel_in || !gyro_in) {
                std::cerr
                    << "Failed to open temporary IMU pairing files\n";
                return false;
            }

            TimedVectorSample accel;
            TimedVectorSample gyro;

            bool have_accel = readTimedVectorSample(accel_in, accel);
            bool have_gyro = readTimedVectorSample(gyro_in, gyro);

            while (have_accel && have_gyro) {
                const std::uint64_t difference_ms =
                    timestampDifferenceMs(
                        accel.epoch_ms,
                        gyro.epoch_ms
                    );

                if (difference_ms <= state.xsens_pair_tolerance_ms) {
                    if (!writeXsensWideRow(state, accel, gyro)) {
                        return false;
                    }

                    have_accel = readTimedVectorSample(accel_in, accel);
                    have_gyro = readTimedVectorSample(gyro_in, gyro);
                    continue;
                }

                if (accel.epoch_ms < gyro.epoch_ms) {
                    state.xsens_unmatched_accel++;
                    have_accel = readTimedVectorSample(accel_in, accel);
                }
                else {
                    state.xsens_unmatched_gyro++;
                    have_gyro = readTimedVectorSample(gyro_in, gyro);
                }
            }

            while (have_accel) {
                state.xsens_unmatched_accel++;
                have_accel = readTimedVectorSample(accel_in, accel);
            }

            while (have_gyro) {
                state.xsens_unmatched_gyro++;
                have_gyro = readTimedVectorSample(gyro_in, gyro);
            }

            accel_in.close();
            gyro_in.close();

            std::error_code remove_error;
            std::filesystem::remove(paths.xsens_accel_temp, remove_error);

            if (remove_error) {
                std::cerr
                    << "WARNING: failed to remove temporary file "
                    << paths.xsens_accel_temp
                    << ": "
                    << remove_error.message()
                    << "\n";
            }

            remove_error.clear();
            std::filesystem::remove(paths.xsens_gyro_temp, remove_error);

            if (remove_error) {
                std::cerr
                    << "WARNING: failed to remove temporary file "
                    << paths.xsens_gyro_temp
                    << ": "
                    << remove_error.message()
                    << "\n";
            }

            return true;
        }

        void writeBatteryDataRow(
            SyncState* state,
            const MblMwData* data
        ) {
            if (state == nullptr) {
                return;
            }

            if (data == nullptr) {
                printUnexpectedData(
                    "battery",
                    data,
                    "data is null"
                );

                return;
            }

            if (data->value == nullptr) {
                printUnexpectedData(
                    "battery",
                    data,
                    "data->value is null"
                );

                return;
            }

            if (data->type_id !=
                MBL_MW_DT_ID_BATTERY_STATE) {
                printUnexpectedData(
                    "battery",
                    data,
                    "expected BATTERY_STATE"
                );

                return;
            }

            const auto* battery =
                static_cast<const MblMwBatteryState*>(
                    data->value
                    );

            std::lock_guard<std::mutex> lock(
                state->csv_mutex
            );

            if (!state->battery_csv.is_open()) {
                printUnexpectedData(
                    "battery",
                    data,
                    "battery_csv is not open"
                );

                return;
            }

            const RowTimestamp timestamp =
                resolveRowTimestampLocked(
                    *state,
                    data->epoch
                );

            state->battery_csv
                << timestamp.epoch_ms
                << ","
                << timestamp.elapsed_ms
                << ","
                << timestamp.utc_timestamp
                << ","
                << battery->voltage
                << ","
                << static_cast<int>(battery->charge)
                << "\n";

            if (!state->battery_csv) {
                std::cerr
                    << "Failed to write battery CSV row\n";

                return;
            }

            state->battery_rows_written++;
        }

        // void writeBatteryDataRow(
        //     SyncState* state,
        //     const MblMwData* data
        // ) {
        //     if (state == nullptr) {
        //         return;
        //     }

        //     if (data == nullptr) {
        //         printUnexpectedData("battery", data, "data is null");
        //         return;
        //     }

        //     if (data->value == nullptr) {
        //         printUnexpectedData("battery", data, "data->value is null");
        //         return;
        //     }

        //     if (data->type_id != MBL_MW_DT_ID_BATTERY_STATE) {
        //         printUnexpectedData("battery", data, "expected BATTERY_STATE");
        //         return;
        //     }

        //     if (!state->battery_csv.is_open()) {
        //         printUnexpectedData("battery", data, "battery_csv is not open");
        //         return;
        //     }

        //     const auto* battery =
        //         static_cast<const MblMwBatteryState*>(data->value);

        //     std::lock_guard<std::mutex> lock(state->csv_mutex);

        //     state->battery_csv
        //         << data->epoch << ","
        //         << battery->voltage << ","
        //         << static_cast<int>(battery->charge)
        //         << "\n";

        //     state->battery_rows_written++;
        // }

        void markDownloadStarted(SyncState* state) {
            if (state != nullptr) {
                state->download_started = true;
            }
        }

        void onAccelLoggerData(void* context, const MblMwData* data) {
            auto* state = static_cast<SyncState*>(context);
            markDownloadStarted(state);
            writeImuDataRow(state, "accel_g", data);
        }

        void onGyroLoggerData(void* context, const MblMwData* data) {
            auto* state = static_cast<SyncState*>(context);
            markDownloadStarted(state);
            writeImuDataRow(state, "gyro_dps", data);
        }

        void onBatteryLoggerData(void* context, const MblMwData* data) {
            auto* state = static_cast<SyncState*>(context);
            markDownloadStarted(state);
            writeBatteryDataRow(state, data);
        }
        // void onAccelLoggerData(void* context, const MblMwData* data) {
        //     writeImuDataRow(static_cast<SyncState*>(context), "accel_g", data);
        // }

        // void onGyroLoggerData(void* context, const MblMwData* data) {
        //     writeImuDataRow(static_cast<SyncState*>(context), "gyro_dps", data);
        // }

        // void onBatteryLoggerData(void* context, const MblMwData* data) {
        //     writeBatteryDataRow(static_cast<SyncState*>(context), data);
        // }

        // void onProgressUpdate(
        //     void* context,
        //     std::uint32_t entries_left,
        //     std::uint32_t total_entries
        // ) {
        //     auto* state = static_cast<SyncState*>(context);

        //     if (state == nullptr) {
        //         return;
        //     }

        //     // std::cerr
        //     // << "\n[progress callback] entries_left="
        //     // << entries_left
        //     // << ", total_entries="
        //     // << total_entries
        //     // << "\n";

        //     state->download_started = true;
        //     state->entries_left = entries_left;
        //     state->total_entries = total_entries;

        //     if (entries_left == 0) {
        //         state->download_done = true;
        //     }
        // }

        void onProgressUpdate(
            void* context,
            std::uint32_t entries_left,
            std::uint32_t total_entries
        ) {
            auto* state =
                static_cast<SyncState*>(context);

            if (state == nullptr) {
                return;
            }

            state->download_started = true;
            state->entries_left = entries_left;
            state->total_entries = total_entries;

            if (state->progress_callback) {
                try {
                    state->progress_callback(
                        entries_left,
                        total_entries
                    );
                }
                catch (...) {
                    /*
                     * Never allow a UI callback exception to
                     * unwind through the MetaWear C callback.
                     */
                }
            }

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
            std::lock_guard<std::mutex> lock(state.csv_mutex);
            if (state.imu_csv.is_open()) {
                state.imu_csv.flush();
                state.imu_csv.close();
            }

            if (state.xsens_csv.is_open()) {
                state.xsens_csv.flush();
                state.xsens_csv.close();
            }

            if (state.xsens_accel_temp.is_open()) {
                state.xsens_accel_temp.flush();
                state.xsens_accel_temp.close();
            }

            if (state.xsens_gyro_temp.is_open()) {
                state.xsens_gyro_temp.flush();
                state.xsens_gyro_temp.close();
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

        void onAnonymousSignalsCreated(
            void* context,
            MblMwMetaWearBoard* board,
            MblMwAnonymousDataSignal** signals,
            std::uint32_t size
        ) {
            (void)board;

            auto* state =
                static_cast<SyncState*>(context);

            if (state == nullptr) {
                return;
            }

            state->anonymous_signals.clear();

            /*
             * The SDK reports discovery failures with signals == nullptr and places the
             * status value in the size argument.  An empty board can also produce
             * nullptr with size == 0, which is a successful discovery of zero loggers.
             */
            if (signals == nullptr) {
                state->anonymous_discovery_status =
                    size == 0
                    ? 0
                    : static_cast<std::int32_t>(size);

                state->anonymous_discovery_done = true;
                return;
            }

            try {
                state->anonymous_signals.assign(
                    signals,
                    signals + size
                );
                state->anonymous_discovery_status = 0;
            }
            catch (...) {
                /*
                 * Never unwind a C++ exception through the MetaWear C callback.
                 */
                state->anonymous_signals.clear();
                state->anonymous_discovery_status = -2;
            }

            state->anonymous_discovery_done = true;
        }

        bool discoverAnonymousSignals(
            headmotion::sdk::MetaWearSdkBridge& bridge,
            SyncState& state,
            int timeout_ms
        ) {
            state.anonymous_signals.clear();
            state.anonymous_discovery_done = false;
            state.anonymous_discovery_status = -999;

            MblMwMetaWearBoard* board =
                bridge.board();

            if (board == nullptr) {
                throw std::runtime_error(
                    "Cannot discover loggers from a null MetaWear board"
                );
            }

            mbl_mw_metawearboard_create_anonymous_datasignals(
                board,
                &state,
                onAnonymousSignalsCreated
            );

            const auto deadline =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(timeout_ms);

            while (
                !state.anonymous_discovery_done.load() &&
                std::chrono::steady_clock::now() < deadline
                ) {
                bridge.pumpOnce(100);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10)
                );
            }

            if (!state.anonymous_discovery_done.load()) {
                state.anonymous_discovery_status = -1;
                return false;
            }

            return
                state.anonymous_discovery_status.load() == 0;
        }

        bool startsWith(
            const std::string& value,
            const std::string& prefix
        ) {
            return
                value.size() >= prefix.size() &&
                value.compare(
                    0,
                    prefix.size(),
                    prefix
                ) == 0;
        }

        enum class AnonymousSignalKind {
            Acceleration,
            AngularVelocity,
            Battery,
            Unknown
        };

        AnonymousSignalKind classifyAnonymousSignal(
            const std::string& identifier
        ) {
            /*
             * Full-vector IMU loggers created by RecordStartCommand are named
             * "acceleration" and "angular-velocity".  Processor chains append
             * ":..." to the root identifier, so accept those as well.
             *
             * Do not treat battery[0] / battery[1] component loggers as the full
             * battery-state logger because writeBatteryDataRow expects
             * MblMwBatteryState.
             */
            if (
                identifier == "acceleration" ||
                startsWith(
                    identifier,
                    "acceleration:"
                )
                ) {
                return AnonymousSignalKind::Acceleration;
            }

            if (
                identifier == "angular-velocity" ||
                startsWith(
                    identifier,
                    "angular-velocity:"
                )
                ) {
                return AnonymousSignalKind::AngularVelocity;
            }

            if (identifier == "battery") {
                return AnonymousSignalKind::Battery;
            }

            return AnonymousSignalKind::Unknown;
        }

    } // namespace

    /*
     * Stateless sync entry point.
     *
     * All logger routing is reconstructed from the connected MMS+ at runtime.
     */
    int runSyncCommand(
        const std::string& port_name,
        const std::string& output_path,
        SyncProgressCallback progress_callback
    ) {
        using namespace std::chrono_literals;

        /*
         * These objects must outlive every MetaWear callback that references them.
         * SyncState is declared before the transport/bridge so it is destroyed last.
         */
        SyncState sync_state;
        sync_state.progress_callback =
            std::move(progress_callback);

        MblMwLogDownloadHandler download_handler = {};

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

        headmotion::sdk::MetaWearSdkBridge bridge(usb);

        /*
         * Stateless initialization:
         *
         * Do not deserialize any previous SDK state.  Build a new SDK board from
         * the connected MMS+ every time sync runs.
         */
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

        /*
         * Ask the physical board which logger routes currently exist.
         *
         * This replaces:
         *   - board_state.bin
         *   - logger_metadata.txt
         *   - saved logger IDs
         */
        std::cout
            << "Discovering logger routes from MMS+ board state\n";

        if (!discoverAnonymousSignals(
            bridge,
            sync_state,
            10000
        )) {
            std::cerr
                << "Anonymous logger discovery failed, status="
                << sync_state.anonymous_discovery_status.load()
                << "\n";

            return 4;
        }

        std::cout
            << "Discovered "
            << sync_state.anonymous_signals.size()
            << " anonymous logger signal";

        if (sync_state.anonymous_signals.size() != 1) {
            std::cout << "s";
        }

        std::cout << ":\n";

        MblMwAnonymousDataSignal* accel_signal = nullptr;
        MblMwAnonymousDataSignal* gyro_signal = nullptr;
        MblMwAnonymousDataSignal* battery_signal = nullptr;

        std::vector<std::string> unknown_signal_identifiers;

        for (
            std::size_t i = 0;
            i < sync_state.anonymous_signals.size();
            ++i
            ) {
            MblMwAnonymousDataSignal* signal =
                sync_state.anonymous_signals[i];

            if (signal == nullptr) {
                std::cout
                    << "  ["
                    << i
                    << "] <null>\n";

                continue;
            }

            const char* identifier_ptr =
                mbl_mw_anonymous_datasignal_get_identifier(
                    signal
                );

            const std::string identifier =
                identifier_ptr != nullptr
                ? std::string(identifier_ptr)
                : std::string{};

            std::cout
                << "  ["
                << i
                << "] "
                << (
                    identifier.empty()
                    ? "<unnamed>"
                    : identifier
                    )
                << "\n";

            switch (
                classifyAnonymousSignal(identifier)
                ) {
            case AnonymousSignalKind::Acceleration:
                if (accel_signal != nullptr) {
                    std::cerr
                        << "Multiple accelerometer logger routes were discovered. "
                        << "Refusing to guess which one contains the head-motion data.\n";

                    return 4;
                }

                accel_signal = signal;
                break;

            case AnonymousSignalKind::AngularVelocity:
                if (gyro_signal != nullptr) {
                    std::cerr
                        << "Multiple gyro logger routes were discovered. "
                        << "Refusing to guess which one contains the head-motion data.\n";

                    return 4;
                }

                gyro_signal = signal;
                break;

            case AnonymousSignalKind::Battery:
                if (battery_signal != nullptr) {
                    std::cerr
                        << "Multiple full battery logger routes were discovered. "
                        << "Refusing to guess which one should be used.\n";

                    return 4;
                }

                battery_signal = signal;
                break;

            case AnonymousSignalKind::Unknown:
                unknown_signal_identifiers.push_back(
                    identifier.empty()
                    ? "<unnamed>"
                    : identifier
                );
                break;
            }
        }

        if (accel_signal == nullptr) {
            std::cerr
                << "No anonymous accelerometer logger was discovered.\n";

            return 4;
        }

        if (gyro_signal == nullptr) {
            std::cerr
                << "No anonymous gyro logger was discovered.\n";

            return 4;
        }

        if (!unknown_signal_identifiers.empty()) {
            std::cerr
                << "WARNING: "
                << unknown_signal_identifiers.size()
                << " unrecognized logger route";

            if (unknown_signal_identifiers.size() != 1) {
                std::cerr << "s";
            }

            std::cerr
                << " will not be written by this sync:\n";

            for (
                const auto& identifier :
                unknown_signal_identifiers
                ) {
                std::cerr
                    << "  "
                    << identifier
                    << "\n";
            }
        }

        const bool battery_enabled =
            battery_signal != nullptr;

        std::cout
            << "Accelerometer logger: discovered\n"
            << "Gyro logger: discovered\n"
            << "Battery logger: "
            << (
                battery_enabled
                ? "discovered"
                : "not present"
                )
            << "\n";

        /*
         * We no longer load sample-rate metadata from the host.  Keep the existing
         * conservative pairing tolerance for the Xsens-compatible export until the
         * sensor configuration is queried directly from the board.
         */
        std::cout
            << "Xsens accel/gyro pairing tolerance: "
            << sync_state.xsens_pair_tolerance_ms
            << " ms\n";

        const std::filesystem::path output_dir{
            output_path
        };

        std::filesystem::create_directories(
            output_dir
        );

        const CsvOutputPaths csv_paths =
            chooseUnusedCsvOutputPaths(
                output_dir
            );

        const auto& imu_path =
            csv_paths.imu;

        const auto& xsens_path =
            csv_paths.xsens;

        const auto& battery_path =
            csv_paths.battery;

        std::cout
            << "IMU output: "
            << imu_path
            << "\n";

        if (!openCsvForAppend(
            sync_state.imu_csv,
            imu_path,
            "epoch_ms,elapsed_ms,utc_timestamp,sensor,x,y,z\n"
        )) {
            return 3;
        }

        std::cout
            << "Xsens-compatible output: "
            << xsens_path
            << "\n";

        if (!openCsvForAppend(
            sync_state.xsens_csv,
            xsens_path,
            "PacketCounter,SampleTimeFine,Euler_X,Euler_Y,Euler_Z,"
            "Acc_X,Acc_Y,Acc_Z,Gyr_X,Gyr_Y,Gyr_Z,elapsed_ms,utc_timestamp\n"
        )) {
            sync_state.imu_csv.close();
            return 3;
        }

        sync_state.xsens_accel_temp.open(
            csv_paths.xsens_accel_temp,
            std::ios::out |
            std::ios::binary |
            std::ios::trunc
        );

        sync_state.xsens_gyro_temp.open(
            csv_paths.xsens_gyro_temp,
            std::ios::out |
            std::ios::binary |
            std::ios::trunc
        );

        if (
            !sync_state.xsens_accel_temp ||
            !sync_state.xsens_gyro_temp
            ) {
            std::cerr
                << "Failed to open temporary IMU pairing files\n";

            closeCsvs(
                sync_state,
                false
            );

            return 3;
        }

        if (battery_enabled) {
            std::cout
                << "Battery output: "
                << battery_path
                << "\n";

            if (!openCsvForAppend(
                sync_state.battery_csv,
                battery_path,
                "epoch_ms,elapsed_ms,utc_timestamp,voltage_mv,charge_percent\n"
            )) {
                closeCsvs(
                    sync_state,
                    false
                );

                return 3;
            }
        }

        MblMwMetaWearBoard* board =
            bridge.board();

        if (board == nullptr) {
            std::cerr
                << "SDK returned a null board after initialization\n";

            closeCsvs(
                sync_state,
                battery_enabled
            );

            return 2;
        }

        /*
         * Flush the current flash page so the newest samples are visible to the
         * download before starting the readout.
         */
        std::cout
            << "Flushing MMS log page before download\n";

        mbl_mw_logging_flush_page(board);
        pumpFor(
            bridge,
            2000
        );

        /*
         * Subscribe directly to the anonymous logger routes discovered from the
         * board.  There are no saved logger IDs in the stateless sync path.
         */
        std::cout
            << "Subscribing anonymous accelerometer logger\n";

        mbl_mw_anonymous_datasignal_subscribe(
            accel_signal,
            &sync_state,
            onAccelLoggerData
        );

        std::cout
            << "Subscribing anonymous gyro logger\n";

        mbl_mw_anonymous_datasignal_subscribe(
            gyro_signal,
            &sync_state,
            onGyroLoggerData
        );

        if (battery_enabled) {
            std::cout
                << "Subscribing anonymous battery logger\n";

            mbl_mw_anonymous_datasignal_subscribe(
                battery_signal,
                &sync_state,
                onBatteryLoggerData
            );
        }

        download_handler.context =
            &sync_state;

        download_handler.received_progress_update =
            onProgressUpdate;

        download_handler.received_unknown_entry =
            onUnknownEntry;

        download_handler.received_unhandled_entry =
            onUnhandledEntry;

        std::cout
            << "Starting log download\n";

        mbl_mw_logging_download(
            board,
            255,
            &download_handler
        );

        constexpr auto IDLE_TIMEOUT =
            std::chrono::minutes(2);

        constexpr auto PROGRESS_PRINT_INTERVAL =
            std::chrono::milliseconds(1000);

        auto last_progress_time =
            std::chrono::steady_clock::now();

        auto last_progress_print_time =
            std::chrono::steady_clock::now();

        bool printed_progress_line = false;

        std::uint64_t last_rows_written =
            totalRowsWritten(sync_state);

        std::uint32_t last_entries_left =
            sync_state.entries_left.load();

        std::uint32_t last_total_entries =
            sync_state.total_entries.load();

        while (!sync_state.download_done.load()) {
            bridge.pumpOnce(10);

            const auto now =
                std::chrono::steady_clock::now();

            const std::uint64_t current_rows_written =
                totalRowsWritten(sync_state);

            const std::uint32_t current_entries_left =
                sync_state.entries_left.load();

            const std::uint32_t current_total_entries =
                sync_state.total_entries.load();

            const bool rows_changed =
                current_rows_written !=
                last_rows_written;

            const bool progress_changed =
                current_entries_left !=
                last_entries_left ||
                current_total_entries !=
                last_total_entries;

            /*
             * Only treat entries_left == 0 as final progress after the SDK has
             * reported a real nonzero total.  Otherwise the default 0/0 startup
             * state would look like a completed transfer.
             */
            const bool sdk_has_progress_total =
                current_total_entries > 0;

            const bool sdk_reports_done =
                sdk_has_progress_total &&
                current_entries_left == 0;

            const bool should_print_progress =
                sync_state.download_started.load() &&
                (
                    now - last_progress_print_time >=
                    PROGRESS_PRINT_INTERVAL ||
                    sdk_reports_done
                    );

            if (should_print_progress) {
                const std::uint64_t imu_rows =
                    sync_state.imu_rows_written.load();

                const std::uint64_t battery_rows =
                    sync_state.battery_rows_written.load();

                const std::uint64_t total_rows =
                    imu_rows +
                    battery_rows;

                std::cout
                    << "\r\033[K";

                std::cout
                    << "Download: "
                    << total_rows
                    << " rows"
                    << " (IMU="
                    << imu_rows
                    << ", battery="
                    << battery_rows
                    << ")";

                if (
                    sdk_has_progress_total &&
                    current_total_entries >=
                    current_entries_left
                    ) {
                    const std::uint32_t entries_downloaded =
                        current_total_entries -
                        current_entries_left;

                    const double sdk_percent =
                        (
                            static_cast<double>(
                                entries_downloaded
                                ) *
                            100.0
                            ) /
                        static_cast<double>(
                            current_total_entries
                            );

                    std::cout
                        << " | SDK: "
                        << std::fixed
                        << std::setprecision(2)
                        << sdk_percent
                        << "% "
                        << "("
                        << entries_downloaded
                        << "/"
                        << current_total_entries
                        << " entries)";
                }
                else {
                    std::cout
                        << " | SDK: waiting for total entry count";
                }

                std::cout
                    << std::flush;

                printed_progress_line = true;
                last_progress_print_time = now;
            }

            if (
                rows_changed ||
                progress_changed
                ) {
                last_progress_time = now;

                last_rows_written =
                    current_rows_written;

                last_entries_left =
                    current_entries_left;

                last_total_entries =
                    current_total_entries;
            }

            if (
                sync_state.download_started.load() &&
                now - last_progress_time >
                IDLE_TIMEOUT
                ) {
                if (printed_progress_line) {
                    std::cout << "\n";
                    printed_progress_line = false;
                }

                std::cerr
                    << "Sync timed out: no download progress for 2 minutes.\n";

                std::cerr
                    << "IMU rows written so far: "
                    << sync_state.imu_rows_written.load()
                    << "\n";

                std::cerr
                    << "Battery rows written so far: "
                    << sync_state.battery_rows_written.load()
                    << "\n";

                std::cerr
                    << "Entries left: "
                    << sync_state.entries_left.load()
                    << " / "
                    << sync_state.total_entries.load()
                    << "\n";

                std::cerr
                    << "Unknown entries: "
                    << sync_state.unknown_entries.load()
                    << "\n";

                std::cerr
                    << "Unhandled entries: "
                    << sync_state.unhandled_entries.load()
                    << "\n";

                std::cerr
                    << "Output directory: "
                    << output_dir
                    << "\n";

                closeCsvs(
                    sync_state,
                    battery_enabled
                );

                return 5;
            }

            if (
                !sync_state.download_started.load() &&
                now - last_progress_time >
                IDLE_TIMEOUT
                ) {
                if (printed_progress_line) {
                    std::cout << "\n";
                    printed_progress_line = false;
                }

                std::cerr
                    << "Sync timed out: download did not start within 2 minutes.\n";

                std::cerr
                    << "IMU rows written so far: "
                    << sync_state.imu_rows_written.load()
                    << "\n";

                std::cerr
                    << "Battery rows written so far: "
                    << sync_state.battery_rows_written.load()
                    << "\n";

                std::cerr
                    << "Output directory: "
                    << output_dir
                    << "\n";

                closeCsvs(
                    sync_state,
                    battery_enabled
                );

                return 5;
            }

            std::this_thread::sleep_for(
                1ms
            );
        }

        if (!finalizeXsensCsv(
            sync_state,
            csv_paths
        )) {
            closeCsvs(
                sync_state,
                battery_enabled
            );

            return 6;
        }

        closeCsvs(
            sync_state,
            battery_enabled
        );

        if (printed_progress_line) {
            std::cout << "\n";
        }

        if (CLEAR_AFTER_SUCCESSFUL_SYNC) {
            std::cout
                << "Clearing downloaded log entries\n";

            mbl_mw_logging_clear_entries(
                board
            );

            pumpFor(
                bridge,
                2000
            );
        }

        std::cout
            << "Sync complete.\n";

        std::cout
            << "IMU rows written: "
            << sync_state.imu_rows_written.load()
            << "\n";

        std::cout
            << "Xsens-compatible rows written: "
            << sync_state.xsens_rows_written.load()
            << "\n";

        std::cout
            << "Unmatched accel samples: "
            << sync_state.xsens_unmatched_accel
            << "\n";

        std::cout
            << "Unmatched gyro samples: "
            << sync_state.xsens_unmatched_gyro
            << "\n";

        if (battery_enabled) {
            std::cout
                << "Battery rows written: "
                << sync_state.battery_rows_written.load()
                << "\n";
        }

        std::cout
            << "Unknown entries: "
            << sync_state.unknown_entries.load()
            << "\n";

        std::cout
            << "Unhandled entries: "
            << sync_state.unhandled_entries.load()
            << "\n";

        std::cout
            << "IMU CSV: "
            << imu_path
            << "\n";

        std::cout
            << "Xsens-compatible CSV: "
            << xsens_path
            << "\n";

        if (battery_enabled) {
            std::cout
                << "Battery CSV: "
                << battery_path
                << "\n";
        }

        return 0;
    }

} // namespace headmotion::app