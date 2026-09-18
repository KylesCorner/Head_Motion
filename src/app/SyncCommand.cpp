/*
 * Developer Notes: SyncCommand
 * ----------------------------
 *
 * Stateless MMS+ log download path.
 *
 * Performance design:
 *   - MetaWear SDK callbacks never format CSV rows or timestamps.
 *   - Callbacks only validate/copy decoded samples into a bounded queue.
 *   - A dedicated writer thread drains the queue, pairs accel/gyro samples,
 *     and writes the Xsens-compatible CSV.
 *   - The legacy long-format imu.csv is disabled by default and can be enabled
 *     by the CLI through the write_imu_csv overload of runSyncCommand.
 *   - UTC timestamps are generated only on the CSV writer thread, never in
 *     the SDK callback path.  Temporary accel/gyro pairing files are gone.
 *
 * Stateless behavior is preserved:
 *   - A fresh SDK board is initialized for every sync.
 *   - Logger routes are reconstructed from anonymous data signals on the MMS+.
 *   - No serialized board state, saved logger IDs, or per-device host metadata
 *     are required.
 */

#include "headmotion/app/Commands.hpp"
#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

extern "C" {
#include "metawear/core/anonymous_datasignal.h"
#include "metawear/core/data.h"
#include "metawear/core/logging.h"
#include "metawear/core/metawearboard.h"
#include "metawear/core/types.h"
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <ctime>
#include <sstream>
#include <utility>
#include <vector>

namespace headmotion::app {

namespace {

constexpr bool CLEAR_AFTER_SUCCESSFUL_SYNC = false;

/*
 * The writer should easily outrun a normal 25-200 Hz recording, but keep the
 * queue bounded so a disk failure cannot consume unbounded RAM.
 *
 * 262,144 records is several minutes of buffering at 200 Hz accel + gyro.
 */
constexpr std::size_t MAX_QUEUED_SAMPLES = 262144;

enum class SampleKind {
    Acceleration,
    AngularVelocity,
    Battery
};

struct QueuedSample {
    SampleKind kind = SampleKind::Acceleration;
    std::int64_t epoch_ms = 0;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    std::uint16_t battery_voltage_mv = 0;
    std::uint8_t battery_charge_percent = 0;
};

struct TimedVectorSample {
    std::int64_t epoch_ms = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct SyncState {
    CommandOutput* output = nullptr;
    bool write_imu_csv = false;
    bool battery_enabled = false;

    std::ofstream imu_csv;
    std::ofstream xsens_csv;
    std::ofstream battery_csv;

    SyncProgressCallback progress_callback;

    /*
     * Writer-thread-only timestamp state.
     *
     * Keep timestamps numeric.  Formatting millions of UTC strings was a major
     * source of CPU work in the old callback path.
     */
    std::optional<std::int64_t> first_sdk_epoch_ms;

    /*
     * Producer/consumer queue.  MetaWear callbacks are producers; csvWriterMain
     * is the only consumer.
     */
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<QueuedSample> sample_queue;
    bool producers_finished = false;

    std::atomic<bool> queue_overflow{ false };
    std::atomic<bool> writer_failed{ false };

    std::mutex writer_error_mutex;
    std::string writer_error;

    /*
     * Anonymous logger discovery state.
     */
    std::atomic<bool> anonymous_discovery_done{ false };
    std::atomic<std::int32_t> anonymous_discovery_status{ -999 };
    std::vector<MblMwAnonymousDataSignal*> anonymous_signals;

    std::atomic<bool> download_started{ false };
    std::atomic<bool> download_done{ false };

    std::atomic<std::uint32_t> entries_left{ 0 };
    std::atomic<std::uint32_t> total_entries{ 0 };

    /*
     * Counters are atomic because the USB/download thread reports them while the
     * writer thread updates them.
     */
    std::atomic<std::uint64_t> imu_samples_received{ 0 };
    std::atomic<std::uint64_t> battery_samples_received{ 0 };

    std::atomic<std::uint64_t> imu_rows_written{ 0 };
    std::atomic<std::uint64_t> xsens_rows_written{ 0 };
    std::atomic<std::uint64_t> battery_rows_written{ 0 };

    std::uint64_t xsens_packet_counter = 0;
    std::uint64_t xsens_unmatched_accel = 0;
    std::uint64_t xsens_unmatched_gyro = 0;
    std::uint32_t xsens_pair_tolerance_ms = 20;

    std::atomic<std::uint64_t> unknown_entries{ 0 };
    std::atomic<std::uint64_t> unhandled_entries{ 0 };
};

struct CsvOutputPaths {
    std::filesystem::path imu;
    std::filesystem::path xsens;
    std::filesystem::path battery;
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
            output_dir / ("battery" + suffix + ".csv")
        };

        /*
         * Reserve the same suffix for the whole session even when imu.csv is not
         * requested.  This keeps related output names aligned.
         */
        if (
            !std::filesystem::exists(candidate.imu) &&
            !std::filesystem::exists(candidate.xsens) &&
            !std::filesystem::exists(candidate.battery)
        ) {
            return candidate;
        }
    }
}

bool openCsv(
    CommandOutput& output,
    std::ofstream& stream,
    const std::filesystem::path& path,
    const char* header
) {
    stream.open(
        path,
        std::ios::out |
        std::ios::binary |
        std::ios::trunc
    );

    if (!stream.is_open()) {
        output.error(
            "csv_open_failed",
            "Failed to open CSV: " + path.string()
        );
        return false;
    }

    stream << header;

    if (!stream) {
        output.error(
            "csv_header_failed",
            "Failed to write CSV header: " + path.string()
        );
        stream.close();
        return false;
    }

    return true;
}

void closeCsvs(SyncState& state) {
    if (state.imu_csv.is_open()) {
        state.imu_csv.flush();
        state.imu_csv.close();
    }

    if (state.xsens_csv.is_open()) {
        state.xsens_csv.flush();
        state.xsens_csv.close();
    }

    if (state.battery_csv.is_open()) {
        state.battery_csv.flush();
        state.battery_csv.close();
    }
}

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
    SyncState* state,
    const char* stream_name,
    const MblMwData* data,
    const char* reason
) {
    if (state == nullptr || state->output == nullptr) {
        return;
    }

    if (data == nullptr) {
        state->output->warning(
            "unexpected_data",
            std::string(stream_name) +
                ": null MblMwData: " +
                reason
        );
        return;
    }

    state->output->event(
        "warning",
        {
            {"code", "unexpected_data"},
            {"stream", stream_name},
            {"reason", reason},
            {"epoch_ms", static_cast<std::int64_t>(data->epoch)},
            {"type_id", static_cast<int>(data->type_id)},
            {"type_name", dataTypeName(data->type_id)},
            {"length", static_cast<int>(data->length)}
        }
    );
}

std::uint64_t timestampDifferenceMs(
    std::int64_t lhs,
    std::int64_t rhs
) {
    return lhs >= rhs
        ? static_cast<std::uint64_t>(lhs - rhs)
        : static_cast<std::uint64_t>(rhs - lhs);
}

void setWriterError(
    SyncState& state,
    std::string message
) {
    {
        std::lock_guard<std::mutex> lock(
            state.writer_error_mutex
        );

        if (state.writer_error.empty()) {
            state.writer_error = std::move(message);
        }
    }

    state.writer_failed = true;
    state.queue_cv.notify_all();
}

std::string writerError(SyncState& state) {
    std::lock_guard<std::mutex> lock(
        state.writer_error_mutex
    );

    return state.writer_error;
}

bool enqueueSample(
    SyncState& state,
    QueuedSample sample
) {
    bool notify_writer = false;

    {
        std::lock_guard<std::mutex> lock(
            state.queue_mutex
        );

        if (
            state.writer_failed.load() ||
            state.producers_finished
        ) {
            return false;
        }

        if (
            state.sample_queue.size() >=
            MAX_QUEUED_SAMPLES
        ) {
            state.queue_overflow = true;
            state.writer_failed = true;

            {
                std::lock_guard<std::mutex> error_lock(
                    state.writer_error_mutex
                );

                if (state.writer_error.empty()) {
                    state.writer_error =
                        "CSV writer queue overflowed; disk/CPU writer "
                        "could not keep up with the MMS+ download";
                }
            }

            notify_writer = true;
        }
        else {
            notify_writer =
                state.sample_queue.empty();

            state.sample_queue.push_back(
                std::move(sample)
            );
        }
    }

    if (notify_writer) {
        state.queue_cv.notify_one();
    }

    return !state.writer_failed.load();
}

void markFirstEpoch(
    SyncState& state,
    std::int64_t epoch_ms
) {
    if (!state.first_sdk_epoch_ms.has_value()) {
        state.first_sdk_epoch_ms = epoch_ms;
    }
}

std::int64_t elapsedMs(
    SyncState& state,
    std::int64_t epoch_ms
) {
    markFirstEpoch(
        state,
        epoch_ms
    );

    return
        epoch_ms -
        *state.first_sdk_epoch_ms;
}

std::string formatUtcTimestamp(
    std::int64_t epoch_ms
) {
    std::int64_t seconds =
        epoch_ms / 1000;

    std::int64_t milliseconds =
        epoch_ms % 1000;

    if (milliseconds < 0) {
        milliseconds += 1000;
        --seconds;
    }

    const std::time_t time_value =
        static_cast<std::time_t>(
            seconds
        );

    std::tm utc_tm{};

#ifdef _WIN32
    if (gmtime_s(
        &utc_tm,
        &time_value
    ) != 0) {
        return "INVALID_UTC";
    }
#else
    if (gmtime_r(
        &time_value,
        &utc_tm
    ) == nullptr) {
        return "INVALID_UTC";
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

    return formatted.str();
}

bool writeLegacyImuRow(
    SyncState& state,
    const QueuedSample& sample,
    const char* sensor_name
) {
    if (!state.write_imu_csv) {
        return true;
    }

    if (!state.imu_csv.is_open()) {
        setWriterError(
            state,
            "Legacy IMU CSV was requested but is not open"
        );
        return false;
    }

    state.imu_csv
        << sample.epoch_ms
        << ","
        << elapsedMs(
            state,
            sample.epoch_ms
        )
        << ","
        << sensor_name
        << ","
        << sample.x
        << ","
        << sample.y
        << ","
        << sample.z
        << "\n";

    if (!state.imu_csv) {
        setWriterError(
            state,
            "Failed while writing legacy IMU CSV"
        );
        return false;
    }

    state.imu_rows_written++;
    return true;
}

bool writeBatteryRow(
    SyncState& state,
    const QueuedSample& sample
) {
    if (!state.battery_enabled) {
        return true;
    }

    if (!state.battery_csv.is_open()) {
        setWriterError(
            state,
            "Battery logger is enabled but battery CSV is not open"
        );
        return false;
    }

    state.battery_csv
        << sample.epoch_ms
        << ","
        << elapsedMs(
            state,
            sample.epoch_ms
        )
        << ","
        << sample.battery_voltage_mv
        << ","
        << static_cast<int>(
            sample.battery_charge_percent
        )
        << "\n";

    if (!state.battery_csv) {
        setWriterError(
            state,
            "Failed while writing battery CSV"
        );
        return false;
    }

    state.battery_rows_written++;
    return true;
}

bool writeXsensWideRow(
    SyncState& state,
    const TimedVectorSample& accel,
    const TimedVectorSample& gyro
) {
    /*
     * UTC formatting happens on the CSV writer thread, never in the SDK
     * callback path.  This preserves the Xsens export timestamp while keeping
     * download callbacks lightweight.
     */
    const std::int64_t elapsed_ms =
        elapsedMs(
            state,
            gyro.epoch_ms
        );

    const std::string utc_timestamp =
        formatUtcTimestamp(
            gyro.epoch_ms
        );

    state.xsens_csv
        << state.xsens_packet_counter++
        << ","
        << gyro.epoch_ms
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
        << elapsed_ms
        << ","
        << utc_timestamp
        << "\n";

    if (!state.xsens_csv) {
        setWriterError(
            state,
            "Failed while writing Xsens-compatible CSV"
        );
        return false;
    }

    state.xsens_rows_written++;
    return true;
}

bool drainPairQueues(
    SyncState& state,
    std::deque<TimedVectorSample>& accel_samples,
    std::deque<TimedVectorSample>& gyro_samples
) {
    while (
        !accel_samples.empty() &&
        !gyro_samples.empty()
    ) {
        const TimedVectorSample& accel =
            accel_samples.front();

        const TimedVectorSample& gyro =
            gyro_samples.front();

        const std::uint64_t difference_ms =
            timestampDifferenceMs(
                accel.epoch_ms,
                gyro.epoch_ms
            );

        if (
            difference_ms <=
            state.xsens_pair_tolerance_ms
        ) {
            if (!writeXsensWideRow(
                state,
                accel,
                gyro
            )) {
                return false;
            }

            accel_samples.pop_front();
            gyro_samples.pop_front();
            continue;
        }

        if (accel.epoch_ms < gyro.epoch_ms) {
            ++state.xsens_unmatched_accel;
            accel_samples.pop_front();
        }
        else {
            ++state.xsens_unmatched_gyro;
            gyro_samples.pop_front();
        }
    }

    return true;
}

void csvWriterMain(
    SyncState* state
) {
    if (state == nullptr) {
        return;
    }

    std::deque<TimedVectorSample> accel_samples;
    std::deque<TimedVectorSample> gyro_samples;
    std::deque<QueuedSample> batch;

    try {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(
                    state->queue_mutex
                );

                state->queue_cv.wait(
                    lock,
                    [state] {
                        return
                            state->writer_failed.load() ||
                            state->producers_finished ||
                            !state->sample_queue.empty();
                    }
                );

                if (
                    state->writer_failed.load() &&
                    state->sample_queue.empty()
                ) {
                    break;
                }

                if (state->sample_queue.empty()) {
                    if (state->producers_finished) {
                        break;
                    }

                    continue;
                }

                batch.clear();
                batch.swap(
                    state->sample_queue
                );
            }

            for (const QueuedSample& sample : batch) {
                if (state->writer_failed.load()) {
                    break;
                }

                /*
                 * Establish the elapsed-time origin from the first callback
                 * sample, independent of which output files are enabled.
                 */
                markFirstEpoch(
                    *state,
                    sample.epoch_ms
                );

                if (
                    sample.kind ==
                    SampleKind::Acceleration
                ) {
                    if (!writeLegacyImuRow(
                        *state,
                        sample,
                        "accel_g"
                    )) {
                        break;
                    }

                    accel_samples.push_back(
                        TimedVectorSample{
                            sample.epoch_ms,
                            sample.x,
                            sample.y,
                            sample.z
                        }
                    );
                }
                else if (
                    sample.kind ==
                    SampleKind::AngularVelocity
                ) {
                    if (!writeLegacyImuRow(
                        *state,
                        sample,
                        "gyro_dps"
                    )) {
                        break;
                    }

                    gyro_samples.push_back(
                        TimedVectorSample{
                            sample.epoch_ms,
                            sample.x,
                            sample.y,
                            sample.z
                        }
                    );
                }
                else {
                    if (!writeBatteryRow(
                        *state,
                        sample
                    )) {
                        break;
                    }

                    continue;
                }

                if (!drainPairQueues(
                    *state,
                    accel_samples,
                    gyro_samples
                )) {
                    break;
                }
            }

            if (state->writer_failed.load()) {
                break;
            }
        }

        /*
         * Any remaining samples cannot be paired after all producers have
         * finished.
         */
        state->xsens_unmatched_accel +=
            accel_samples.size();

        state->xsens_unmatched_gyro +=
            gyro_samples.size();

        if (state->imu_csv.is_open()) {
            state->imu_csv.flush();

            if (!state->imu_csv) {
                setWriterError(
                    *state,
                    "Failed while flushing legacy IMU CSV"
                );
            }
        }

        if (state->xsens_csv.is_open()) {
            state->xsens_csv.flush();

            if (!state->xsens_csv) {
                setWriterError(
                    *state,
                    "Failed while flushing Xsens-compatible CSV"
                );
            }
        }

        if (state->battery_csv.is_open()) {
            state->battery_csv.flush();

            if (!state->battery_csv) {
                setWriterError(
                    *state,
                    "Failed while flushing battery CSV"
                );
            }
        }
    }
    catch (const std::exception& error) {
        setWriterError(
            *state,
            std::string(
                "CSV writer thread failed: "
            ) +
            error.what()
        );
    }
    catch (...) {
        setWriterError(
            *state,
            "CSV writer thread failed with an unknown exception"
        );
    }
}

void finishWriter(
    SyncState& state,
    std::thread& writer_thread
) {
    {
        std::lock_guard<std::mutex> lock(
            state.queue_mutex
        );

        state.producers_finished = true;
    }

    state.queue_cv.notify_all();

    if (writer_thread.joinable()) {
        writer_thread.join();
    }
}

void markDownloadStarted(
    SyncState* state
) {
    if (state != nullptr) {
        state->download_started = true;
    }
}

bool validateVectorData(
    SyncState* state,
    const char* sensor,
    const MblMwData* data
) {
    if (data == nullptr) {
        printUnexpectedData(
            state,
            sensor,
            data,
            "data is null"
        );
        return false;
    }

    if (data->value == nullptr) {
        printUnexpectedData(
            state,
            sensor,
            data,
            "data->value is null"
        );
        return false;
    }

    if (
        data->type_id !=
        MBL_MW_DT_ID_CARTESIAN_FLOAT
    ) {
        printUnexpectedData(
            state,
            sensor,
            data,
            "expected CARTESIAN_FLOAT"
        );
        return false;
    }

    if (
        data->length <
        sizeof(MblMwCartesianFloat)
    ) {
        printUnexpectedData(
            state,
            sensor,
            data,
            "value is shorter than MblMwCartesianFloat"
        );
        return false;
    }

    return true;
}

void enqueueVectorData(
    SyncState* state,
    SampleKind kind,
    const char* sensor,
    const MblMwData* data
) {
    if (state == nullptr) {
        return;
    }

    markDownloadStarted(state);

    if (!validateVectorData(
        state,
        sensor,
        data
    )) {
        return;
    }

    const auto* value =
        static_cast<const MblMwCartesianFloat*>(
            data->value
        );

    QueuedSample sample;
    sample.kind = kind;
    sample.epoch_ms = data->epoch;
    sample.x = value->x;
    sample.y = value->y;
    sample.z = value->z;

    state->imu_samples_received++;

    enqueueSample(
        *state,
        std::move(sample)
    );
}

void onAccelLoggerData(
    void* context,
    const MblMwData* data
) {
    enqueueVectorData(
        static_cast<SyncState*>(context),
        SampleKind::Acceleration,
        "accel_g",
        data
    );
}

void onGyroLoggerData(
    void* context,
    const MblMwData* data
) {
    enqueueVectorData(
        static_cast<SyncState*>(context),
        SampleKind::AngularVelocity,
        "gyro_dps",
        data
    );
}

void onBatteryLoggerData(
    void* context,
    const MblMwData* data
) {
    auto* state =
        static_cast<SyncState*>(context);

    if (state == nullptr) {
        return;
    }

    markDownloadStarted(state);

    if (data == nullptr) {
        printUnexpectedData(
            state,
            "battery",
            data,
            "data is null"
        );
        return;
    }

    if (data->value == nullptr) {
        printUnexpectedData(
            state,
            "battery",
            data,
            "data->value is null"
        );
        return;
    }

    if (
        data->type_id !=
        MBL_MW_DT_ID_BATTERY_STATE
    ) {
        printUnexpectedData(
            state,
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

    QueuedSample sample;
    sample.kind = SampleKind::Battery;
    sample.epoch_ms = data->epoch;
    sample.battery_voltage_mv =
        battery->voltage;
    sample.battery_charge_percent =
        battery->charge;

    state->battery_samples_received++;

    enqueueSample(
        *state,
        std::move(sample)
    );
}

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

    if (state->output != nullptr) {
        state->output->progress(
            entries_left,
            total_entries
        );
    }

    if (state->progress_callback) {
        try {
            state->progress_callback(
                entries_left,
                total_entries
            );
        }
        catch (...) {
            /*
             * Never unwind a C++ exception through a MetaWear C callback.
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

    auto* state =
        static_cast<SyncState*>(context);

    if (state != nullptr) {
        state->unknown_entries++;
    }
}

void onUnhandledEntry(
    void* context,
    const MblMwData* data
) {
    (void)data;

    auto* state =
        static_cast<SyncState*>(context);

    if (state != nullptr) {
        state->unhandled_entries++;
    }
}

std::uint64_t totalSamplesReceived(
    const SyncState& state
) {
    return
        state.imu_samples_received.load() +
        state.battery_samples_received.load();
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
     * SDK discovery failures use signals == nullptr and put the status value
     * in size.  nullptr + 0 is a legitimate "no loggers" result.
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
    if (
        identifier == "acceleration" ||
        startsWith(
            identifier,
            "acceleration:"
        )
    ) {
        return
            AnonymousSignalKind::Acceleration;
    }

    if (
        identifier == "angular-velocity" ||
        startsWith(
            identifier,
            "angular-velocity:"
        )
    ) {
        return
            AnonymousSignalKind::AngularVelocity;
    }

    if (identifier == "battery") {
        return
            AnonymousSignalKind::Battery;
    }

    return
        AnonymousSignalKind::Unknown;
}

} // namespace

/*
 * Full sync implementation.
 *
 * write_imu_csv == false is the normal/GUI path.
 * write_imu_csv == true additionally creates the legacy long-format imu.csv.
 */
int runSyncCommand(
    const std::string& port_name,
    const std::string& output_path,
    bool write_imu_csv,
    CommandOutput& output,
    SyncProgressCallback progress_callback
) {
    using namespace std::chrono_literals;

    SyncState sync_state;
    sync_state.output = &output;
    sync_state.write_imu_csv = write_imu_csv;
    sync_state.progress_callback = std::move(progress_callback);

    output.started(
        {
            {"port", port_name},
            {"output_directory", output_path},
            {"write_legacy_imu_csv", write_imu_csv}
        }
    );

    MblMwLogDownloadHandler download_handler = {};

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    output.status("opening_port");

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

    output.status("initializing_sdk");

    const bool initialized =
        bridge.initialize(5000);

    if (!initialized) {
        output.event(
            "error",
            {
                {"code", "sdk_init_failed"},
                {"message", "SDK initialization failed"},
                {"sdk_status", bridge.initializeStatus()}
            }
        );
        output.completed(false, 2);
        return 2;
    }

    output.status("discovering_loggers");

    if (!discoverAnonymousSignals(
        bridge,
        sync_state,
        10000
    )) {
        output.event(
            "error",
            {
                {"code", "logger_discovery_failed"},
                {"message", "Anonymous logger discovery failed"},
                {
                    "sdk_status",
                    static_cast<int>(
                        sync_state.anonymous_discovery_status.load()
                    )
                }
            }
        );
        output.completed(false, 4);
        return 4;
    }

    output.event(
        "logger_discovery",
        {
            {
                "count",
                static_cast<std::uint64_t>(
                    sync_state.anonymous_signals.size()
                )
            }
        }
    );

    MblMwAnonymousDataSignal* accel_signal = nullptr;
    MblMwAnonymousDataSignal* gyro_signal = nullptr;
    MblMwAnonymousDataSignal* battery_signal = nullptr;

    std::vector<std::string>
        unknown_signal_identifiers;

    for (
        std::size_t i = 0;
        i < sync_state.anonymous_signals.size();
        ++i
    ) {
        MblMwAnonymousDataSignal* signal =
            sync_state.anonymous_signals[i];

        if (signal == nullptr) {
            output.event(
                "warning",
                {
                    {"code", "null_logger_signal"},
                    {
                        "index",
                        static_cast<std::uint64_t>(i)
                    }
                }
            );
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

        const AnonymousSignalKind kind =
            classifyAnonymousSignal(identifier);

        const char* kind_name = "unknown";

        switch (kind) {
        case AnonymousSignalKind::Acceleration:
            kind_name = "acceleration";
            break;
        case AnonymousSignalKind::AngularVelocity:
            kind_name = "angular_velocity";
            break;
        case AnonymousSignalKind::Battery:
            kind_name = "battery";
            break;
        case AnonymousSignalKind::Unknown:
            break;
        }

        output.event(
            "logger",
            {
                {
                    "index",
                    static_cast<std::uint64_t>(i)
                },
                {
                    "identifier",
                    identifier.empty()
                        ? std::string("<unnamed>")
                        : identifier
                },
                {"kind", kind_name}
            }
        );

        switch (kind) {
        case AnonymousSignalKind::Acceleration:
            if (accel_signal != nullptr) {
                output.error(
                    "multiple_accelerometer_loggers",
                    "Multiple accelerometer logger routes were discovered"
                );
                output.completed(false, 4);
                return 4;
            }

            accel_signal = signal;
            break;

        case AnonymousSignalKind::AngularVelocity:
            if (gyro_signal != nullptr) {
                output.error(
                    "multiple_gyro_loggers",
                    "Multiple gyro logger routes were discovered"
                );
                output.completed(false, 4);
                return 4;
            }

            gyro_signal = signal;
            break;

        case AnonymousSignalKind::Battery:
            if (battery_signal != nullptr) {
                output.error(
                    "multiple_battery_loggers",
                    "Multiple full battery logger routes were discovered"
                );
                output.completed(false, 4);
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
        output.error(
            "accelerometer_logger_missing",
            "No anonymous accelerometer logger was discovered"
        );
        output.completed(false, 4);
        return 4;
    }

    if (gyro_signal == nullptr) {
        output.error(
            "gyro_logger_missing",
            "No anonymous gyro logger was discovered"
        );
        output.completed(false, 4);
        return 4;
    }

    for (
        const std::string& identifier :
        unknown_signal_identifiers
    ) {
        output.event(
            "warning",
            {
                {"code", "unknown_logger"},
                {"identifier", identifier},
                {
                    "message",
                    "Logger will not be written by this sync"
                }
            }
        );
    }

    sync_state.battery_enabled =
        battery_signal != nullptr;

    output.event(
        "configuration",
        {
            {"accelerometer_logger", true},
            {"gyro_logger", true},
            {
                "battery_logger",
                sync_state.battery_enabled
            },
            {
                "xsens_pair_tolerance_ms",
                static_cast<std::uint64_t>(
                    sync_state.xsens_pair_tolerance_ms
                )
            },
            {
                "write_legacy_imu_csv",
                sync_state.write_imu_csv
            },
            {"xsens_utc_timestamp", true}
        }
    );

    output.status("preparing_output");

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

    if (!openCsv(
        output,
        sync_state.xsens_csv,
        xsens_path,
        "PacketCounter,SampleTimeFine,Euler_X,Euler_Y,Euler_Z,"
        "Acc_X,Acc_Y,Acc_Z,Gyr_X,Gyr_Y,Gyr_Z,elapsed_ms,utc_timestamp\n"
    )) {
        output.completed(false, 3);
        return 3;
    }

    output.outputFile(
        "xsens_csv",
        xsens_path.string()
    );

    if (sync_state.write_imu_csv) {
        if (!openCsv(
            output,
            sync_state.imu_csv,
            imu_path,
            "epoch_ms,elapsed_ms,sensor,x,y,z\n"
        )) {
            closeCsvs(sync_state);
            output.completed(false, 3);
            return 3;
        }

        output.outputFile(
            "legacy_imu_csv",
            imu_path.string()
        );
    }

    if (sync_state.battery_enabled) {
        if (!openCsv(
            output,
            sync_state.battery_csv,
            battery_path,
            "epoch_ms,elapsed_ms,voltage_mv,charge_percent\n"
        )) {
            closeCsvs(sync_state);
            output.completed(false, 3);
            return 3;
        }

        output.outputFile(
            "battery_csv",
            battery_path.string()
        );
    }

    MblMwMetaWearBoard* board =
        bridge.board();

    if (board == nullptr) {
        output.error(
            "null_sdk_board",
            "SDK returned a null board after initialization"
        );
        closeCsvs(sync_state);
        output.completed(false, 2);
        return 2;
    }

    /*
     * Start the writer before subscribing so callbacks only need to enqueue
     * samples.  UTC formatting happens on this writer thread.
     */
    std::thread writer_thread(
        csvWriterMain,
        &sync_state
    );

    output.status("flushing_log_page");

    mbl_mw_logging_flush_page(board);

    pumpFor(
        bridge,
        2000
    );

    output.status("subscribing_loggers");

    mbl_mw_anonymous_datasignal_subscribe(
        accel_signal,
        &sync_state,
        onAccelLoggerData
    );

    mbl_mw_anonymous_datasignal_subscribe(
        gyro_signal,
        &sync_state,
        onGyroLoggerData
    );

    if (sync_state.battery_enabled) {
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

    output.status("downloading");

    mbl_mw_logging_download(
        board,
        255,
        &download_handler
    );

    constexpr auto IDLE_TIMEOUT =
        std::chrono::minutes(2);

    auto last_progress_time =
        std::chrono::steady_clock::now();

    std::uint64_t last_samples_received =
        totalSamplesReceived(
            sync_state
        );

    std::uint32_t last_entries_left =
        sync_state.entries_left.load();

    std::uint32_t last_total_entries =
        sync_state.total_entries.load();

    int transfer_error_code = 0;
    std::string transfer_error_id;
    std::string transfer_error_message;

    while (!sync_state.download_done.load()) {
        bridge.pumpOnce(10);

        const auto now =
            std::chrono::steady_clock::now();

        if (sync_state.writer_failed.load()) {
            transfer_error_code = 6;
            transfer_error_id =
                "csv_writer_failed";
            transfer_error_message =
                writerError(sync_state);
            break;
        }

        const std::uint64_t current_samples_received =
            totalSamplesReceived(
                sync_state
            );

        const std::uint32_t current_entries_left =
            sync_state.entries_left.load();

        const std::uint32_t current_total_entries =
            sync_state.total_entries.load();

        const bool samples_changed =
            current_samples_received !=
            last_samples_received;

        const bool progress_changed =
            current_entries_left !=
                last_entries_left ||
            current_total_entries !=
                last_total_entries;

        if (
            samples_changed ||
            progress_changed
        ) {
            last_progress_time = now;

            last_samples_received =
                current_samples_received;

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
            transfer_error_code = 5;
            transfer_error_id =
                "download_stalled";
            transfer_error_message =
                "No download progress for 2 minutes";
            break;
        }

        if (
            !sync_state.download_started.load() &&
            now - last_progress_time >
                IDLE_TIMEOUT
        ) {
            transfer_error_code = 5;
            transfer_error_id =
                "download_not_started";
            transfer_error_message =
                "Download did not start within 2 minutes";
            break;
        }
    }

    output.status("finalizing_output");

    finishWriter(
        sync_state,
        writer_thread
    );

    closeCsvs(
        sync_state
    );

    if (transfer_error_code != 0) {
        output.event(
            "error",
            {
                {"code", transfer_error_id},
                {"message", transfer_error_message},
                {
                    "imu_samples_received",
                    sync_state.imu_samples_received.load()
                },
                {
                    "battery_samples_received",
                    sync_state.battery_samples_received.load()
                },
                {
                    "entries_left",
                    static_cast<std::uint64_t>(
                        sync_state.entries_left.load()
                    )
                },
                {
                    "entries_total",
                    static_cast<std::uint64_t>(
                        sync_state.total_entries.load()
                    )
                },
                {
                    "unknown_entries",
                    sync_state.unknown_entries.load()
                },
                {
                    "unhandled_entries",
                    sync_state.unhandled_entries.load()
                }
            }
        );
        output.completed(false, transfer_error_code);
        return transfer_error_code;
    }

    if (sync_state.writer_failed.load()) {
        output.error(
            "csv_writer_failed",
            writerError(sync_state)
        );
        output.completed(false, 6);
        return 6;
    }

    if (CLEAR_AFTER_SUCCESSFUL_SYNC) {
        output.status("clearing_downloaded_logs");

        mbl_mw_logging_clear_entries(
            board
        );

        pumpFor(
            bridge,
            2000
        );
    }

    output.event(
        "summary",
        {
            {
                "imu_samples_received",
                sync_state.imu_samples_received.load()
            },
            {
                "xsens_rows_written",
                sync_state.xsens_rows_written.load()
            },
            {
                "unmatched_accel_samples",
                sync_state.xsens_unmatched_accel
            },
            {
                "unmatched_gyro_samples",
                sync_state.xsens_unmatched_gyro
            },
            {
                "legacy_imu_rows_written",
                sync_state.imu_rows_written.load()
            },
            {
                "battery_rows_written",
                sync_state.battery_rows_written.load()
            },
            {
                "unknown_entries",
                sync_state.unknown_entries.load()
            },
            {
                "unhandled_entries",
                sync_state.unhandled_entries.load()
            }
        }
    );

    output.completed(true, 0);
    return 0;
}

/*
 * Xsens-only overload for callers that provide their own per-operation sink.
 */
int runSyncCommand(
    const std::string& port_name,
    const std::string& output_path,
    CommandOutput& output,
    SyncProgressCallback progress_callback
) {
    return runSyncCommand(
        port_name,
        output_path,
        false,
        output,
        std::move(progress_callback)
    );
}

/*
 * Compatibility overload used by existing CLI callers.
 * JSONL is written to stdout, but no global stream redirection is performed.
 */
int runSyncCommand(
    const std::string& port_name,
    const std::string& output_path,
    bool write_imu_csv,
    SyncProgressCallback progress_callback
) {
    CommandOutput output("sync");

    return runSyncCommand(
        port_name,
        output_path,
        write_imu_csv,
        output,
        std::move(progress_callback)
    );
}

/*
 * Backward-compatible Xsens-only entry point used by the current GUI.
 */
int runSyncCommand(
    const std::string& port_name,
    const std::string& output_path,
    SyncProgressCallback progress_callback
) {
    CommandOutput output("sync");

    return runSyncCommand(
        port_name,
        output_path,
        false,
        output,
        std::move(progress_callback)
    );
}

} // namespace headmotion::app
