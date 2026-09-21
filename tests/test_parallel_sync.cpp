/*
 * Parallel synthetic sync stress test.
 *
 * No physical MMS+ hardware is required.
 *
 * Each virtual device gets its own:
 *   - SyncState
 *   - producer stream
 *   - writer thread
 *   - accel/gyro queues
 *   - Xsens CSV
 *
 * All virtual devices generate a large number of samples concurrently.
 *
 * This verifies:
 *   - independent SyncState instances
 *   - concurrent CSV writer threads
 *   - accel/gyro pairing
 *   - queue integrity
 *   - no queue overflows
 *   - no writer failures
 *   - correct row counts
 *   - contiguous packet counters
 *   - output isolation between virtual devices
 *
 * Environment variables:
 *
 *   HEADMOTION_PARALLEL_DEVICES
 *       Number of simultaneous virtual MMS+ devices.
 *       Default: 4
 *
 *   HEADMOTION_PARALLEL_PAIRS
 *       Number of accel/gyro pairs generated per device.
 *       Default: 200000
 *
 * Example:
 *
 *   HEADMOTION_PARALLEL_DEVICES=8 \
 *   HEADMOTION_PARALLEL_PAIRS=500000 \
 *   ./build/linux-native-debug/headmotion_test_parallel_sync
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

constexpr std::size_t DEFAULT_DEVICE_COUNT = 4;

constexpr std::uint64_t DEFAULT_PAIR_COUNT =
    200000;

constexpr std::size_t QUEUE_HIGH_WATER =
    65536;

constexpr const char* XSENS_HEADER =
    "PacketCounter,SampleTimeFine,"
    "Euler_X,Euler_Y,Euler_Z,"
    "Acc_X,Acc_Y,Acc_Z,"
    "Gyr_X,Gyr_Y,Gyr_Z,"
    "elapsed_ms,utc_timestamp";

std::mutex console_mutex;

void logLine(
    std::size_t device_index,
    const std::string& message
) {
    std::lock_guard<std::mutex> lock(
        console_mutex
    );

    std::cout
        << "[device "
        << device_index
        << "] "
        << message
        << '\n';
}

std::uint64_t readUintEnvironment(
    const char* name,
    std::uint64_t default_value
) {
    const char* value =
        std::getenv(name);

    if (
        value == nullptr ||
        *value == '\0'
    ) {
        return default_value;
    }

    try {
        return std::stoull(value);
    }
    catch (...) {
        std::cerr
            << "Invalid value for "
            << name
            << ": "
            << value
            << '\n';

        std::exit(2);
    }
}

std::filesystem::path makeOutputRoot()
{
    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const auto path =
        std::filesystem::temp_directory_path() /
        (
            "headmotion_parallel_sync_" +
            std::to_string(timestamp)
        );

    std::filesystem::create_directories(
        path
    );

    return path;
}

struct DeviceResult {
    std::size_t index = 0;

    std::filesystem::path csv_path;

    std::uint64_t expected_pairs = 0;
    std::uint64_t received_samples = 0;
    std::uint64_t rows_written = 0;

    std::uint64_t unmatched_accel = 0;
    std::uint64_t unmatched_gyro = 0;

    bool queue_overflow = false;
    bool writer_failed = false;

    std::string writer_error;

    std::uint64_t csv_rows = 0;

    bool packet_counters_valid = false;
    bool header_valid = false;

    bool passed = false;
};

} // namespace


/*
 * SyncCommand.cpp places its implementation details inside an
 * anonymous namespace nested in headmotion::app.
 *
 * Because this test includes SyncCommand.cpp directly, we can exercise
 * those internals from within headmotion::app.
 */
namespace headmotion::app {

bool waitForQueueBelow(
    SyncState& state,
    std::size_t threshold
) {
    constexpr auto TIMEOUT =
        std::chrono::seconds(30);

    const auto deadline =
        std::chrono::steady_clock::now() +
        TIMEOUT;

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        if (state.writer_failed.load()) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(
                state.queue_mutex
            );

            if (
                state.sample_queue.size() <
                threshold
            ) {
                return true;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1)
        );
    }

    return false;
}


bool validateCsv(
    DeviceResult& result
) {
    std::ifstream csv(
        result.csv_path
    );

    if (!csv.is_open()) {
        logLine(
            result.index,
            "FAIL: could not reopen CSV"
        );

        return false;
    }

    std::string line;

    /*
     * Validate header.
     */
    if (!std::getline(csv, line)) {
        logLine(
            result.index,
            "FAIL: CSV is empty"
        );

        return false;
    }

    if (line != XSENS_HEADER) {
        logLine(
            result.index,
            "FAIL: incorrect CSV header"
        );

        return false;
    }

    result.header_valid = true;

    /*
     * Validate every packet counter.
     *
     * This catches:
     *   - missing rows
     *   - duplicate rows
     *   - corrupted ordering
     *   - cross-device state contamination
     */
    std::uint64_t expected_packet = 0;

    while (std::getline(csv, line)) {
        const std::size_t comma =
            line.find(',');

        if (
            comma ==
            std::string::npos
        ) {
            logLine(
                result.index,
                "FAIL: malformed CSV row"
            );

            return false;
        }

        std::uint64_t packet_counter = 0;

        try {
            packet_counter =
                std::stoull(
                    line.substr(
                        0,
                        comma
                    )
                );
        }
        catch (...) {
            logLine(
                result.index,
                "FAIL: invalid packet counter"
            );

            return false;
        }

        if (
            packet_counter !=
            expected_packet
        ) {
            logLine(
                result.index,
                "FAIL: packet counter discontinuity: "
                "expected " +
                std::to_string(
                    expected_packet
                ) +
                ", got " +
                std::to_string(
                    packet_counter
                )
            );

            return false;
        }

        ++expected_packet;
    }

    result.csv_rows =
        expected_packet;

    result.packet_counters_valid =
        true;

    if (
        result.csv_rows !=
        result.expected_pairs
    ) {
        logLine(
            result.index,
            "FAIL: expected " +
            std::to_string(
                result.expected_pairs
            ) +
            " CSV rows, got " +
            std::to_string(
                result.csv_rows
            )
        );

        return false;
    }

    return true;
}


void runVirtualDevice(
    std::size_t device_index,
    std::uint64_t pair_count,
    const std::filesystem::path& output_root,
    std::barrier<>& start_barrier,
    DeviceResult& result
) {
    result.index =
        device_index;

    result.expected_pairs =
        pair_count;

    result.csv_path =
        output_root /
        (
            "MT_virtual_" +
            std::to_string(
                device_index
            ) +
            ".csv"
        );

    logLine(
        device_index,
        "preparing"
    );

    SyncState state;

    /*
     * Every virtual device gets its own Xsens output stream.
     */
    state.xsens_csv.open(
        result.csv_path,
        std::ios::out |
        std::ios::binary |
        std::ios::trunc
    );

    if (!state.xsens_csv.is_open()) {
        logLine(
            device_index,
            "FAIL: could not open CSV"
        );

        result.writer_failed =
            true;

        return;
    }

    state.xsens_csv
        << XSENS_HEADER
        << '\n';

    /*
     * Start a dedicated writer thread exactly as a real sync does.
     */
    std::thread writer_thread(
        csvWriterMain,
        &state
    );

    /*
     * Make every virtual MMS+ begin producing data simultaneously.
     */
    start_barrier.arrive_and_wait();

    logLine(
        device_index,
        "starting synthetic download"
    );

    /*
     * Give every virtual device a unique epoch range.
     *
     * This isn't required by SyncState, but makes accidental
     * cross-device data contamination easier to identify while
     * debugging.
     */
    const std::int64_t base_epoch_ms =
        1760000000000LL +
        static_cast<std::int64_t>(
            device_index *
            100000000ULL
        );

    bool producer_failed =
        false;

    for (
        std::uint64_t i = 0;
        i < pair_count;
        ++i
    ) {
        /*
         * Simulate 200 Hz data:
         *
         *     1000 ms / 200 Hz = 5 ms/sample
         */
        const std::int64_t epoch_ms =
            base_epoch_ms +
            static_cast<std::int64_t>(
                i * 5
            );

        /*
         * --------------------------------------------
         * Accelerometer callback
         * --------------------------------------------
         */
        MblMwCartesianFloat accel{};

        /*
         * Encode the device index into the synthetic data.
         *
         * This makes each virtual device's samples distinct.
         */
        accel.x =
            static_cast<float>(
                device_index
            ) +
            static_cast<float>(
                i % 100
            ) *
            0.001f;

        accel.y =
            static_cast<float>(
                i % 200
            ) *
            0.001f;

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
         * --------------------------------------------
         * Gyroscope callback
         * --------------------------------------------
         */
        MblMwCartesianFloat gyro{};

        gyro.x =
            static_cast<float>(
                device_index * 1000
            ) +
            static_cast<float>(
                i % 300
            );

        gyro.y =
            static_cast<float>(
                i % 400
            );

        gyro.z =
            static_cast<float>(
                i % 500
            );

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

        /*
         * Bail immediately if the writer encountered an error.
         */
        if (state.writer_failed.load()) {
            producer_failed =
                true;

            break;
        }

        /*
         * The synthetic producer can generate samples MUCH faster
         * than USB hardware could.
         *
         * Add backpressure periodically so this test stresses
         * concurrency rather than trivially overflowing the queue
         * because a CPU loop can produce millions of callbacks/sec.
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
                QUEUE_HIGH_WATER
            ) {
                if (
                    !waitForQueueBelow(
                        state,
                        QUEUE_HIGH_WATER / 2
                    )
                ) {
                    producer_failed =
                        true;

                    break;
                }
            }
        }
    }

    /*
     * Tell the writer that no more samples will arrive and wait
     * for it to completely drain the queue.
     */
    finishWriter(
        state,
        writer_thread
    );

    closeCsvs(
        state
    );

    /*
     * Capture results before SyncState is destroyed.
     */
    result.received_samples =
        state.imu_samples_received.load();

    result.rows_written =
        state.xsens_rows_written.load();

    result.unmatched_accel =
        state.xsens_unmatched_accel;

    result.unmatched_gyro =
        state.xsens_unmatched_gyro;

    result.queue_overflow =
        state.queue_overflow.load();

    result.writer_failed =
        state.writer_failed.load();

    result.writer_error =
        writerError(state);

    if (producer_failed) {
        logLine(
            device_index,
            "FAIL: producer stopped early"
        );

        return;
    }

    if (result.writer_failed) {
        logLine(
            device_index,
            "FAIL: writer failed: " +
            result.writer_error
        );

        return;
    }

    if (result.queue_overflow) {
        logLine(
            device_index,
            "FAIL: queue overflow"
        );

        return;
    }

    const std::uint64_t expected_samples =
        pair_count *
        2;

    if (
        result.received_samples !=
        expected_samples
    ) {
        logLine(
            device_index,
            "FAIL: expected " +
            std::to_string(
                expected_samples
            ) +
            " samples, got " +
            std::to_string(
                result.received_samples
            )
        );

        return;
    }

    if (
        result.rows_written !=
        pair_count
    ) {
        logLine(
            device_index,
            "FAIL: expected " +
            std::to_string(
                pair_count
            ) +
            " rows, got " +
            std::to_string(
                result.rows_written
            )
        );

        return;
    }

    if (
        result.unmatched_accel != 0
    ) {
        logLine(
            device_index,
            "FAIL: unmatched accel samples: " +
            std::to_string(
                result.unmatched_accel
            )
        );

        return;
    }

    if (
        result.unmatched_gyro != 0
    ) {
        logLine(
            device_index,
            "FAIL: unmatched gyro samples: " +
            std::to_string(
                result.unmatched_gyro
            )
        );

        return;
    }

    /*
     * Validate the actual CSV on disk.
     */
    if (!validateCsv(result)) {
        return;
    }

    result.passed =
        true;

    logLine(
        device_index,
        "PASS: " +
        std::to_string(
            expected_samples
        ) +
        " samples, " +
        std::to_string(
            pair_count
        ) +
        " paired rows"
    );
}

} // namespace headmotion::app


int main()
{
    const std::size_t device_count =
        static_cast<std::size_t>(
            readUintEnvironment(
                "HEADMOTION_PARALLEL_DEVICES",
                DEFAULT_DEVICE_COUNT
            )
        );

    const std::uint64_t pair_count =
        readUintEnvironment(
            "HEADMOTION_PARALLEL_PAIRS",
            DEFAULT_PAIR_COUNT
        );

    if (device_count < 2) {
        std::cerr
            << "HEADMOTION_PARALLEL_DEVICES must be >= 2\n";

        return 2;
    }

    if (pair_count == 0) {
        std::cerr
            << "HEADMOTION_PARALLEL_PAIRS must be > 0\n";

        return 2;
    }

    const std::filesystem::path output_root =
        makeOutputRoot();

    std::cout
        << "========================================\n"
        << "HeadMotion parallel sync stress test\n"
        << "========================================\n"
        << "Virtual devices: "
        << device_count
        << '\n'
        << "Pairs/device:   "
        << pair_count
        << '\n'
        << "Samples/device: "
        << pair_count * 2
        << '\n'
        << "Total samples:  "
        << pair_count *
            2 *
            device_count
        << '\n'
        << "Output:         "
        << output_root
        << "\n\n";

    /*
     * Every virtual device waits here until all workers are ready.
     */
    std::barrier start_barrier(
        static_cast<std::ptrdiff_t>(
            device_count
        )
    );

    std::vector<
        std::unique_ptr<DeviceResult>
    > results;

    results.reserve(
        device_count
    );

    for (
        std::size_t i = 0;
        i < device_count;
        ++i
    ) {
        results.push_back(
            std::make_unique<DeviceResult>()
        );
    }

    std::vector<std::thread>
        workers;

    workers.reserve(
        device_count
    );

    const auto start_time =
        std::chrono::steady_clock::now();

    /*
     * Start one complete sync pipeline per virtual MMS+.
     */
    for (
        std::size_t i = 0;
        i < device_count;
        ++i
    ) {
        workers.emplace_back(
            [
                i,
                pair_count,
                &output_root,
                &start_barrier,
                &results
            ] {
                headmotion::app::
                    runVirtualDevice(
                        i,
                        pair_count,
                        output_root,
                        start_barrier,
                        *results[i]
                    );
            }
        );
    }

    for (auto& worker : workers) {
        worker.join();
    }

    const auto end_time =
        std::chrono::steady_clock::now();

    const double elapsed_seconds =
        std::chrono::duration<double>(
            end_time -
            start_time
        ).count();

    /*
     * ---------------------------------------------------------
     * Final report
     * ---------------------------------------------------------
     */

    bool all_passed =
        true;

    std::uint64_t total_samples =
        0;

    std::uint64_t total_rows =
        0;

    std::cout
        << "\n"
        << "========================================\n"
        << "Results\n"
        << "========================================\n";

    for (const auto& result : results) {
        std::cout
            << "\nVirtual device "
            << result->index
            << '\n'
            << "  received samples: "
            << result->received_samples
            << '\n'
            << "  rows written:     "
            << result->rows_written
            << '\n'
            << "  CSV rows:         "
            << result->csv_rows
            << '\n'
            << "  unmatched accel:  "
            << result->unmatched_accel
            << '\n'
            << "  unmatched gyro:   "
            << result->unmatched_gyro
            << '\n'
            << "  queue overflow:   "
            << (
                result->queue_overflow
                    ? "YES"
                    : "no"
            )
            << '\n'
            << "  writer failure:   "
            << (
                result->writer_failed
                    ? "YES"
                    : "no"
            )
            << '\n'
            << "  packet counters:  "
            << (
                result->packet_counters_valid
                    ? "valid"
                    : "INVALID"
            )
            << '\n'
            << "  result:           "
            << (
                result->passed
                    ? "PASS"
                    : "FAIL"
            )
            << '\n';

        total_samples +=
            result->received_samples;

        total_rows +=
            result->rows_written;

        if (!result->passed) {
            all_passed =
                false;
        }
    }

    std::cout
        << "\n"
        << "========================================\n"
        << "Summary\n"
        << "========================================\n"
        << "Virtual devices: "
        << device_count
        << '\n'
        << "Total samples:   "
        << total_samples
        << '\n'
        << "Total CSV rows:  "
        << total_rows
        << '\n'
        << "Elapsed time:    "
        << elapsed_seconds
        << " seconds\n";

    if (elapsed_seconds > 0.0) {
        std::cout
            << "Throughput:      "
            << static_cast<double>(
                total_samples
            ) /
                elapsed_seconds
            << " samples/sec\n";
    }

    if (!all_passed) {
        std::cout
            << "\nPARALLEL SYNC TEST FAILED\n"
            << "Artifacts preserved at:\n  "
            << output_root
            << '\n';

        return 1;
    }

    /*
     * Successful runs can generate fairly large temporary CSVs,
     * so clean them up automatically.
     */
    std::error_code error;

    std::filesystem::remove_all(
        output_root,
        error
    );

    std::cout
        << "\nPARALLEL SYNC TEST PASSED\n";

    return 0;
}