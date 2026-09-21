/*
 * Parallel sync failure-isolation test.
 *
 * No MMS+ hardware required.
 *
 * Runs four independent synthetic sync pipelines concurrently:
 *
 *   device 0 -> normal
 *   device 1 -> simulated CSV writer failure
 *   device 2 -> normal
 *   device 3 -> malformed accelerometer callbacks
 *
 * The test verifies that failures in devices 1 and 3 do not affect
 * devices 0 and 2.
 *
 * Environment:
 *
 *   HEADMOTION_FAILURE_ISOLATION_PAIRS
 *
 * Default:
 *   100000 pairs for each normal workload
 */

#include "../src/app/SyncCommand.cpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t DEFAULT_PAIR_COUNT =
    100000;

constexpr const char* XSENS_HEADER =
    "PacketCounter,SampleTimeFine,"
    "Euler_X,Euler_Y,Euler_Z,"
    "Acc_X,Acc_Y,Acc_Z,"
    "Gyr_X,Gyr_Y,Gyr_Z,"
    "elapsed_ms,utc_timestamp";

std::mutex console_mutex;


enum class Scenario {
    Normal,
    WriterFailure,
    MalformedInput
};


struct DeviceResult {
    std::size_t index = 0;

    Scenario scenario =
        Scenario::Normal;

    std::uint64_t requested_pairs = 0;

    std::uint64_t samples_received = 0;
    std::uint64_t rows_written = 0;

    std::uint64_t unmatched_accel = 0;
    std::uint64_t unmatched_gyro = 0;

    std::uint64_t warning_events = 0;

    bool writer_failed = false;
    bool queue_overflow = false;

    std::string writer_error;

    std::uint64_t csv_rows = 0;

    bool expected_behavior = false;

    std::filesystem::path csv_path;
};


void logLine(
    std::size_t device,
    const std::string& message
) {
    std::lock_guard<std::mutex> lock(
        console_mutex
    );

    std::cout
        << "[device "
        << device
        << "] "
        << message
        << '\n';
}


std::uint64_t pairCount()
{
    const char* value =
        std::getenv(
            "HEADMOTION_FAILURE_ISOLATION_PAIRS"
        );

    if (
        value == nullptr ||
        *value == '\0'
    ) {
        return DEFAULT_PAIR_COUNT;
    }

    try {
        return std::stoull(value);
    }
    catch (...) {
        std::cerr
            << "Invalid "
            << "HEADMOTION_FAILURE_ISOLATION_PAIRS\n";

        std::exit(2);
    }
}


std::filesystem::path makeOutputDirectory()
{
    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const auto path =
        std::filesystem::temp_directory_path() /
        (
            "headmotion_failure_isolation_" +
            std::to_string(timestamp)
        );

    std::filesystem::create_directories(
        path
    );

    return path;
}


std::uint64_t countCsvRows(
    const std::filesystem::path& path
) {
    std::ifstream file(path);

    if (!file.is_open()) {
        return 0;
    }

    std::string line;

    /*
     * Skip header.
     */
    if (!std::getline(file, line)) {
        return 0;
    }

    std::uint64_t rows = 0;

    while (std::getline(file, line)) {
        ++rows;
    }

    return rows;
}

} // namespace


namespace headmotion::app {

void enqueueGoodPair(
    SyncState& state,
    std::size_t device_index,
    std::uint64_t sample_index
) {
    const std::int64_t epoch_ms =
        1760000000000LL +
        static_cast<std::int64_t>(
            device_index *
            100000000ULL
        ) +
        static_cast<std::int64_t>(
            sample_index * 5
        );

    /*
     * Accelerometer
     */
    MblMwCartesianFloat accel{};

    accel.x =
        static_cast<float>(
            device_index
        ) +
        0.1f;

    accel.y =
        static_cast<float>(
            sample_index % 100
        );

    accel.z =
        1.0f;

    MblMwData accel_data{};

    accel_data.epoch =
        epoch_ms;

    accel_data.value =
        &accel;

    accel_data.type_id =
        MBL_MW_DT_ID_CARTESIAN_FLOAT;

    accel_data.length =
        sizeof(
            MblMwCartesianFloat
        );

    onAccelLoggerData(
        &state,
        &accel_data
    );

    /*
     * Gyroscope
     */
    MblMwCartesianFloat gyro{};

    gyro.x =
        static_cast<float>(
            device_index * 1000
        ) +
        10.0f;

    gyro.y =
        static_cast<float>(
            sample_index % 200
        );

    gyro.z =
        30.0f;

    MblMwData gyro_data{};

    gyro_data.epoch =
        epoch_ms;

    gyro_data.value =
        &gyro;

    gyro_data.type_id =
        MBL_MW_DT_ID_CARTESIAN_FLOAT;

    gyro_data.length =
        sizeof(
            MblMwCartesianFloat
        );

    onGyroLoggerData(
        &state,
        &gyro_data
    );
}


void enqueueMalformedPair(
    SyncState& state,
    std::size_t device_index,
    std::uint64_t sample_index
) {
    const std::int64_t epoch_ms =
        1760000000000LL +
        static_cast<std::int64_t>(
            device_index *
            100000000ULL
        ) +
        static_cast<std::int64_t>(
            sample_index * 5
        );

    /*
     * Deliberately malformed accelerometer callback.
     *
     * onAccelLoggerData expects:
     *
     *     MBL_MW_DT_ID_CARTESIAN_FLOAT
     *
     * but we provide:
     *
     *     MBL_MW_DT_ID_FLOAT
     */
    float invalid_accel =
        123.0f;

    MblMwData accel_data{};

    accel_data.epoch =
        epoch_ms;

    accel_data.value =
        &invalid_accel;

    accel_data.type_id =
        MBL_MW_DT_ID_FLOAT;

    accel_data.length =
        sizeof(float);

    onAccelLoggerData(
        &state,
        &accel_data
    );

    /*
     * Gyro remains valid.
     *
     * This lets us prove that the malformed accel sample is
     * rejected rather than corrupting the queue.
     */
    MblMwCartesianFloat gyro{};

    gyro.x = 10.0f;
    gyro.y = 20.0f;
    gyro.z = 30.0f;

    MblMwData gyro_data{};

    gyro_data.epoch =
        epoch_ms;

    gyro_data.value =
        &gyro;

    gyro_data.type_id =
        MBL_MW_DT_ID_CARTESIAN_FLOAT;

    gyro_data.length =
        sizeof(
            MblMwCartesianFloat
        );

    onGyroLoggerData(
        &state,
        &gyro_data
    );
}


bool waitForWriterFailure(
    SyncState& state
) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        2s;

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        if (state.writer_failed.load()) {
            return true;
        }

        std::this_thread::sleep_for(
            1ms
        );
    }

    return state.writer_failed.load();
}


void runVirtualDevice(
    DeviceResult& result,
    const std::filesystem::path& output_directory,
    std::barrier<>& start_barrier
) {
    /*
     * Capture warning events independently for each virtual device.
     */
    headmotion::app::CommandOutput output(
        "sync",
        [&result](
            const CommandEvent& event
        ) {
            if (
                event.type ==
                "warning"
            ) {
                ++result.warning_events;
            }
        }
    );

    SyncState state;

    state.output =
        &output;

    result.csv_path =
        output_directory /
        (
            "device_" +
            std::to_string(
                result.index
            ) +
            ".csv"
        );

    state.xsens_csv.open(
        result.csv_path,
        std::ios::out |
        std::ios::binary |
        std::ios::trunc
    );

    if (!state.xsens_csv.is_open()) {
        logLine(
            result.index,
            "could not open output file"
        );

        result.expected_behavior =
            false;

        return;
    }

    state.xsens_csv
        << XSENS_HEADER
        << '\n';

    /*
     * Device 1 simulates a hard storage/write failure.
     *
     * We leave the file physically open but place the stream into
     * bad state. The real writer code will then discover the error
     * through exactly the same `!state.xsens_csv` check used during
     * an actual disk/write failure.
     */
    if (
        result.scenario ==
        Scenario::WriterFailure
    ) {
        state.xsens_csv.setstate(
            std::ios::badbit
        );
    }

    std::thread writer_thread(
        csvWriterMain,
        &state
    );

    /*
     * All four virtual devices start together.
     */
    start_barrier.arrive_and_wait();

    switch (result.scenario) {

    /*
     * =====================================================
     * NORMAL DEVICE
     * =====================================================
     */
    case Scenario::Normal:

        logLine(
            result.index,
            "running normal sync"
        );

        for (
            std::uint64_t i = 0;
            i < result.requested_pairs;
            ++i
        ) {
            enqueueGoodPair(
                state,
                result.index,
                i
            );

            if (state.writer_failed.load()) {
                break;
            }

            /*
             * Don't allow the synthetic producer to become wildly
             * faster than the writer. This test is about failure
             * isolation, not queue overflow.
             */
            if ((i % 4096) == 0) {
                std::size_t queue_size = 0;

                {
                    std::lock_guard<std::mutex> lock(
                        state.queue_mutex
                    );

                    queue_size =
                        state.sample_queue.size();
                }

                if (
                    queue_size >
                    65536
                ) {
                    std::this_thread::sleep_for(
                        2ms
                    );
                }
            }
        }

        break;


    /*
     * =====================================================
     * WRITER FAILURE DEVICE
     * =====================================================
     */
    case Scenario::WriterFailure:

        logLine(
            result.index,
            "injecting writer failure"
        );

        /*
         * Only one pair is necessary.
         *
         * The writer attempts to emit the row, discovers the bad
         * output stream, and should enter writer_failed state.
         */
        enqueueGoodPair(
            state,
            result.index,
            0
        );

        waitForWriterFailure(
            state
        );

        break;


    /*
     * =====================================================
     * MALFORMED INPUT DEVICE
     * =====================================================
     */
    case Scenario::MalformedInput:

        logLine(
            result.index,
            "injecting malformed callbacks"
        );

        for (
            std::uint64_t i = 0;
            i < result.requested_pairs;
            ++i
        ) {
            enqueueMalformedPair(
                state,
                result.index,
                i
            );

            if (state.writer_failed.load()) {
                break;
            }
        }

        break;
    }


    finishWriter(
        state,
        writer_thread
    );

    closeCsvs(
        state
    );


    /*
     * -----------------------------------------------------
     * Capture final state
     * -----------------------------------------------------
     */

    result.samples_received =
        state.imu_samples_received.load();

    result.rows_written =
        state.xsens_rows_written.load();

    result.unmatched_accel =
        state.xsens_unmatched_accel;

    result.unmatched_gyro =
        state.xsens_unmatched_gyro;

    result.writer_failed =
        state.writer_failed.load();

    result.queue_overflow =
        state.queue_overflow.load();

    result.writer_error =
        writerError(state);

    result.csv_rows =
        countCsvRows(
            result.csv_path
        );


    /*
     * -----------------------------------------------------
     * Validate expected behavior
     * -----------------------------------------------------
     */

    switch (result.scenario) {

    case Scenario::Normal:
    {
        const std::uint64_t expected_samples =
            result.requested_pairs *
            2;

        result.expected_behavior =
            !result.writer_failed &&
            !result.queue_overflow &&
            result.warning_events == 0 &&
            result.samples_received ==
                expected_samples &&
            result.rows_written ==
                result.requested_pairs &&
            result.csv_rows ==
                result.requested_pairs &&
            result.unmatched_accel == 0 &&
            result.unmatched_gyro == 0;

        break;
    }


    case Scenario::WriterFailure:

        /*
         * This device is SUPPOSED to fail.
         *
         * The isolation test passes if the failure is detected
         * cleanly and remains local to this SyncState.
         */
        result.expected_behavior =
            result.writer_failed &&
            !result.queue_overflow &&
            !result.writer_error.empty();

        break;


    case Scenario::MalformedInput:

        /*
         * Every accel callback is rejected.
         * Every gyro callback remains valid.
         *
         * Therefore:
         *
         *   rows written     = 0
         *   valid samples    = number of gyros
         *   unmatched gyro   = number of gyros
         *   warnings         > 0
         *
         * Importantly, malformed data should NOT crash the writer
         * or poison another device.
         */
        result.expected_behavior =
            !result.writer_failed &&
            !result.queue_overflow &&
            result.warning_events > 0 &&
            result.rows_written == 0 &&
            result.csv_rows == 0 &&
            result.samples_received ==
                result.requested_pairs &&
            result.unmatched_accel == 0 &&
            result.unmatched_gyro ==
                result.requested_pairs;

        break;
    }


    if (result.expected_behavior) {
        logLine(
            result.index,
            "expected outcome observed"
        );
    }
    else {
        logLine(
            result.index,
            "UNEXPECTED outcome"
        );
    }
}

} // namespace headmotion::app


int main()
{
    const std::uint64_t pairs =
        pairCount();

    if (pairs == 0) {
        std::cerr
            << "Pair count must be greater than zero\n";

        return 2;
    }

    const std::filesystem::path output_directory =
        makeOutputDirectory();

    std::cout
        << "========================================\n"
        << "HeadMotion Parallel Failure Isolation\n"
        << "========================================\n\n"
        << "Pairs/workload: "
        << pairs
        << '\n'
        << "Output: "
        << output_directory
        << "\n\n"
        << "device 0: normal\n"
        << "device 1: forced writer failure\n"
        << "device 2: normal\n"
        << "device 3: malformed input\n\n";


    /*
     * Give device 2 a slightly different workload.
     *
     * This makes cross-device state contamination easier to catch:
     * device 0 and device 2 should not accidentally produce identical
     * row counts simply because all workloads happened to be equal.
     */
    std::vector<DeviceResult> results(4);

    results[0].index =
        0;

    results[0].scenario =
        Scenario::Normal;

    results[0].requested_pairs =
        pairs;


    results[1].index =
        1;

    results[1].scenario =
        Scenario::WriterFailure;

    results[1].requested_pairs =
        1;


    results[2].index =
        2;

    results[2].scenario =
        Scenario::Normal;

    results[2].requested_pairs =
        pairs + 137;


    results[3].index =
        3;

    results[3].scenario =
        Scenario::MalformedInput;

    results[3].requested_pairs =
        pairs;


    /*
     * All four workers begin their failure/normal workloads at the
     * same time.
     */
    std::barrier start_barrier(
        static_cast<std::ptrdiff_t>(
            results.size()
        )
    );


    std::vector<std::thread> workers;

    workers.reserve(
        results.size()
    );


    for (auto& result : results) {
        workers.emplace_back(
            [
                &result,
                &output_directory,
                &start_barrier
            ] {
                headmotion::app::
                    runVirtualDevice(
                        result,
                        output_directory,
                        start_barrier
                    );
            }
        );
    }


    for (auto& worker : workers) {
        worker.join();
    }


    /*
     * =========================================================
     * Report
     * =========================================================
     */

    std::cout
        << "\n"
        << "========================================\n"
        << "Results\n"
        << "========================================\n";


    bool all_expected =
        true;


    for (const auto& result : results) {
        std::cout
            << "\nDevice "
            << result.index
            << '\n'
            << "  samples received: "
            << result.samples_received
            << '\n'
            << "  rows written:     "
            << result.rows_written
            << '\n'
            << "  CSV rows:         "
            << result.csv_rows
            << '\n'
            << "  warnings:         "
            << result.warning_events
            << '\n'
            << "  unmatched accel:  "
            << result.unmatched_accel
            << '\n'
            << "  unmatched gyro:   "
            << result.unmatched_gyro
            << '\n'
            << "  queue overflow:   "
            << (
                result.queue_overflow
                    ? "YES"
                    : "no"
            )
            << '\n'
            << "  writer failed:    "
            << (
                result.writer_failed
                    ? "YES"
                    : "no"
            )
            << '\n';


        if (!result.writer_error.empty()) {
            std::cout
                << "  writer error:     "
                << result.writer_error
                << '\n';
        }


        std::cout
            << "  outcome:          "
            << (
                result.expected_behavior
                    ? "EXPECTED"
                    : "UNEXPECTED"
            )
            << '\n';


        if (!result.expected_behavior) {
            all_expected =
                false;
        }
    }


    /*
     * Additional explicit isolation assertions.
     *
     * Even though devices 1 and 3 experienced failures/bad data,
     * both normal devices must remain completely healthy.
     */
    const bool normal_devices_healthy =
        results[0].expected_behavior &&
        results[2].expected_behavior;


    if (!normal_devices_healthy) {
        all_expected =
            false;

        std::cerr
            << "\nFAIL: failure leaked into a normal device\n";
    }


    if (!results[1].writer_failed) {
        all_expected =
            false;

        std::cerr
            << "\nFAIL: writer failure was not isolated/detected\n";
    }


    if (
        results[0].writer_failed ||
        results[2].writer_failed
    ) {
        all_expected =
            false;

        std::cerr
            << "\nFAIL: writer failure contaminated "
            << "another SyncState\n";
    }


    if (
        results[0].warning_events != 0 ||
        results[2].warning_events != 0
    ) {
        all_expected =
            false;

        std::cerr
            << "\nFAIL: malformed-data warning leaked "
            << "into another device\n";
    }


    if (!all_expected) {
        std::cerr
            << "\nPARALLEL FAILURE ISOLATION TEST FAILED\n"
            << "Artifacts preserved at:\n  "
            << output_directory
            << '\n';

        return 1;
    }


    /*
     * Everything behaved exactly as expected.
     *
     * Remove temporary CSVs on successful runs.
     */
    std::error_code error;

    std::filesystem::remove_all(
        output_directory,
        error
    );


    std::cout
        << "\n"
        << "PASS: device 0 remained healthy\n"
        << "PASS: device 1 writer failure stayed isolated\n"
        << "PASS: device 2 remained healthy\n"
        << "PASS: device 3 malformed input stayed isolated\n"
        << "PASS: no cross-device failure contamination\n\n"
        << "PARALLEL FAILURE ISOLATION TEST PASSED\n";


    return 0;
}