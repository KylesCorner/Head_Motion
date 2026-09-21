/*
 * Large synthetic sync stress test.
 *
 * Exercises:
 *   MetaWear-style callbacks
 *       -> bounded sample queue
 *       -> CSV writer thread
 *       -> accel/gyro pairing
 *       -> Xsens CSV output
 *
 * This intentionally includes SyncCommand.cpp so the test can exercise
 * the internal sync data path without requiring physical MMS+ hardware.
 *
 * Long-term, the writer/queue should be extracted into its own internal
 * component and tested directly.
 */

#include "../src/app/SyncCommand.cpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr std::uint64_t DEFAULT_PAIR_COUNT = 200000;
constexpr std::size_t QUEUE_HIGH_WATER = 65536;

constexpr const char* XSENS_HEADER =
    "PacketCounter,SampleTimeFine,Euler_X,Euler_Y,Euler_Z,"
    "Acc_X,Acc_Y,Acc_Z,Gyr_X,Gyr_Y,Gyr_Z,elapsed_ms,utc_timestamp";

std::uint64_t pairCountFromEnvironment()
{
    const char* value =
        std::getenv("HEADMOTION_TEST_SYNC_PAIRS");

    if (value == nullptr || *value == '\0') {
        return DEFAULT_PAIR_COUNT;
    }

    try {
        return std::stoull(value);
    }
    catch (...) {
        std::cerr
            << "Invalid HEADMOTION_TEST_SYNC_PAIRS: "
            << value
            << '\n';

        std::exit(2);
    }
}

std::filesystem::path makeTempDirectory()
{
    const auto now =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const auto path =
        std::filesystem::temp_directory_path() /
        (
            "headmotion_sync_large_" +
            std::to_string(now)
        );

    std::filesystem::create_directories(path);

    return path;
}

} // namespace

/*
 * Place the actual test inside headmotion::app so the names from
 * SyncCommand.cpp's anonymous namespace are visible by unqualified lookup.
 */
namespace headmotion::app {

bool waitForQueueBelow(
    SyncState& state,
    std::size_t limit
)
{
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

            if (state.sample_queue.size() < limit) {
                return true;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1)
        );
    }

    return false;
}

int runLargeSyncTest()
{
    const std::uint64_t pair_count =
        pairCountFromEnvironment();

    std::cout
        << "Large sync stress test\n"
        << "Accel/gyro pairs: "
        << pair_count
        << '\n'
        << "Total IMU samples: "
        << pair_count * 2
        << '\n';

    /*
     * The default test processes 400,000 samples, which is larger
     * than MAX_QUEUED_SAMPLES (262,144).  They are not all queued
     * simultaneously; this verifies that the writer drains the queue
     * correctly over a transfer larger than its total buffer capacity.
     */
    if (
        pair_count * 2 <=
        MAX_QUEUED_SAMPLES
    ) {
        std::cerr
            << "WARNING: test input is smaller than "
            << "MAX_QUEUED_SAMPLES="
            << MAX_QUEUED_SAMPLES
            << '\n';
    }

    const std::filesystem::path temp_dir =
        makeTempDirectory();

    const std::filesystem::path xsens_path =
        temp_dir / "xsens_large.csv";

    std::cout
        << "Output: "
        << xsens_path
        << '\n';

    SyncState state;

    state.xsens_csv.open(
        xsens_path,
        std::ios::out |
        std::ios::binary |
        std::ios::trunc
    );

    if (!state.xsens_csv.is_open()) {
        std::cerr
            << "FAIL: could not open test CSV\n";

        return 1;
    }

    state.xsens_csv
        << XSENS_HEADER
        << '\n';

    std::thread writer_thread(
        csvWriterMain,
        &state
    );

    constexpr std::int64_t BASE_EPOCH_MS =
        1760000000000LL;

    bool producer_failed = false;

    for (
        std::uint64_t i = 0;
        i < pair_count;
        ++i
    ) {
        /*
         * 200 Hz -> one pair every 5 ms.
         */
        const std::int64_t epoch_ms =
            BASE_EPOCH_MS +
            static_cast<std::int64_t>(i * 5);

        MblMwCartesianFloat accel{};
        accel.x =
            static_cast<float>(i % 100) * 0.01f;
        accel.y =
            static_cast<float>(i % 200) * 0.01f;
        accel.z =
            1.0f;

        MblMwData accel_data{};
        accel_data.epoch = epoch_ms;
        accel_data.value = &accel;
        accel_data.type_id =
            MBL_MW_DT_ID_CARTESIAN_FLOAT;
        accel_data.length =
            sizeof(MblMwCartesianFloat);

        onAccelLoggerData(
            &state,
            &accel_data
        );

        MblMwCartesianFloat gyro{};
        gyro.x =
            static_cast<float>(i % 300);
        gyro.y =
            static_cast<float>(i % 400);
        gyro.z =
            static_cast<float>(i % 500);

        MblMwData gyro_data{};
        gyro_data.epoch = epoch_ms;
        gyro_data.value = &gyro;
        gyro_data.type_id =
            MBL_MW_DT_ID_CARTESIAN_FLOAT;
        gyro_data.length =
            sizeof(MblMwCartesianFloat);

        onGyroLoggerData(
            &state,
            &gyro_data
        );

        if (state.writer_failed.load()) {
            producer_failed = true;
            break;
        }

        /*
         * Synthetic generation is much faster than a real MMS+.
         *
         * Prevent the producer from dumping hundreds of thousands of
         * records into the queue instantaneously.  This still creates
         * substantial bursts while approximating the backpressure from
         * an actual USB transfer.
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
                if (!waitForQueueBelow(
                    state,
                    QUEUE_HIGH_WATER / 2
                )) {
                    producer_failed = true;
                    break;
                }
            }
        }
    }

    finishWriter(
        state,
        writer_thread
    );

    closeCsvs(state);

    if (producer_failed) {
        std::cerr
            << "FAIL: producer stopped early\n"
            << "Writer error: "
            << writerError(state)
            << '\n';

        return 1;
    }

    if (state.writer_failed.load()) {
        std::cerr
            << "FAIL: writer failed: "
            << writerError(state)
            << '\n';

        return 1;
    }

    if (state.queue_overflow.load()) {
        std::cerr
            << "FAIL: sync queue overflowed\n";

        return 1;
    }

    const std::uint64_t expected_samples =
        pair_count * 2;

    if (
        state.imu_samples_received.load() !=
        expected_samples
    ) {
        std::cerr
            << "FAIL: expected "
            << expected_samples
            << " received IMU samples, got "
            << state.imu_samples_received.load()
            << '\n';

        return 1;
    }

    if (
        state.xsens_rows_written.load() !=
        pair_count
    ) {
        std::cerr
            << "FAIL: expected "
            << pair_count
            << " Xsens rows, got "
            << state.xsens_rows_written.load()
            << '\n';

        return 1;
    }

    if (state.xsens_unmatched_accel != 0) {
        std::cerr
            << "FAIL: unmatched accel samples: "
            << state.xsens_unmatched_accel
            << '\n';

        return 1;
    }

    if (state.xsens_unmatched_gyro != 0) {
        std::cerr
            << "FAIL: unmatched gyro samples: "
            << state.xsens_unmatched_gyro
            << '\n';

        return 1;
    }

    /*
     * Validate the physical CSV too, not just the internal counters.
     */
    std::ifstream csv(xsens_path);

    if (!csv.is_open()) {
        std::cerr
            << "FAIL: could not reopen output CSV\n";

        return 1;
    }

    std::string line;

    if (!std::getline(csv, line)) {
        std::cerr
            << "FAIL: CSV is empty\n";

        return 1;
    }

    if (line != XSENS_HEADER) {
        std::cerr
            << "FAIL: incorrect CSV header\n"
            << "Expected: "
            << XSENS_HEADER
            << '\n'
            << "Actual:   "
            << line
            << '\n';

        return 1;
    }

    std::uint64_t csv_rows = 0;

    while (std::getline(csv, line)) {
        const std::size_t comma =
            line.find(',');

        if (comma == std::string::npos) {
            std::cerr
                << "FAIL: malformed CSV row "
                << csv_rows
                << '\n';

            return 1;
        }

        const std::uint64_t packet_counter =
            std::stoull(
                line.substr(
                    0,
                    comma
                )
            );

        if (packet_counter != csv_rows) {
            std::cerr
                << "FAIL: packet counter discontinuity\n"
                << "Expected: "
                << csv_rows
                << '\n'
                << "Actual:   "
                << packet_counter
                << '\n';

            return 1;
        }

        ++csv_rows;
    }

    if (csv_rows != pair_count) {
        std::cerr
            << "FAIL: CSV contains "
            << csv_rows
            << " rows; expected "
            << pair_count
            << '\n';

        return 1;
    }

    std::cout
        << "PASS: "
        << expected_samples
        << " IMU samples processed\n"
        << "PASS: "
        << pair_count
        << " accel/gyro pairs written\n"
        << "PASS: zero unmatched samples\n"
        << "PASS: zero queue overflows\n"
        << "PASS: packet counters are contiguous\n";

    /*
     * Remove the ~tens-of-MB temporary CSV after successful validation.
     */
    std::error_code error;
    std::filesystem::remove_all(
        temp_dir,
        error
    );

    return 0;
}

} // namespace headmotion::app

int main()
{
    return headmotion::app::runLargeSyncTest();
}