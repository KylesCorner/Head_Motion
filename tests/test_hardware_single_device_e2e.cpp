/*
 * Single-device MMS+ hardware end-to-end integration test.
 *
 * REQUIRES REAL HARDWARE.
 *
 * This test is DESTRUCTIVE:
 *   - resets the MMS+ before the test
 *   - clears existing logs
 *   - records new data
 *   - downloads that data
 *   - resets the board again afterward
 *
 * The test is opt-in.
 *
 * Required:
 *
 *   HEADMOTION_HW_E2E_PORT
 *
 * Example:
 *
 *   HEADMOTION_HW_E2E_PORT=/dev/serial/by-id/... \
 *   ./build/linux-native-debug/headmotion_test_hardware_single_device_e2e
 *
 * Optional:
 *
 *   HEADMOTION_HW_E2E_SECONDS=5
 *   HEADMOTION_HW_E2E_BATTERY_INTERVAL=0
 *
 * Battery interval 0 disables battery logging.
 */

#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/app/Commands.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using headmotion::app::CommandEvent;
using headmotion::app::CommandOutput;

constexpr float SAMPLE_RATE_HZ =
    50.0f;

constexpr std::uint64_t DEFAULT_RECORD_SECONDS =
    5;

constexpr std::uint64_t DEFAULT_BATTERY_INTERVAL =
    0;

constexpr int SKIP_RETURN_CODE =
    77;

constexpr const char* XSENS_HEADER =
    "PacketCounter,SampleTimeFine,"
    "Euler_X,Euler_Y,Euler_Z,"
    "Acc_X,Acc_Y,Acc_Z,"
    "Gyr_X,Gyr_Y,Gyr_Z,"
    "elapsed_ms,utc_timestamp";

constexpr const char* LEGACY_HEADER =
    "epoch_ms,elapsed_ms,sensor,x,y,z";

constexpr const char* BATTERY_HEADER =
    "epoch_ms,elapsed_ms,voltage_mv,charge_percent";


int failures = 0;


/*
 * ============================================================
 * Basic reporting
 * ============================================================
 */

void fail(
    const std::string& message
) {
    ++failures;

    std::cerr
        << "FAIL: "
        << message
        << '\n';
}


void pass(
    const std::string& message
) {
    std::cout
        << "PASS: "
        << message
        << '\n';
}


/*
 * ============================================================
 * Command event capture
 * ============================================================
 */

class EventCapture {
public:
    CommandOutput::Sink sink()
    {
        return [this](
            const CommandEvent& event
        ) {
            std::lock_guard<std::mutex> lock(
                mutex_
            );

            events_.push_back(
                event
            );
        };
    }


    std::vector<CommandEvent> snapshot() const
    {
        std::lock_guard<std::mutex> lock(
            mutex_
        );

        return events_;
    }


private:
    mutable std::mutex mutex_;

    std::vector<CommandEvent>
        events_;
};


std::optional<std::string> stringField(
    const CommandEvent& event,
    const std::string& key
) {
    for (const auto& field : event.fields) {
        if (field.key != key) {
            continue;
        }

        if (
            const auto* value =
                std::get_if<std::string>(
                    &field.value
                )
        ) {
            return *value;
        }
    }

    return std::nullopt;
}


std::optional<std::uint64_t> uintField(
    const CommandEvent& event,
    const std::string& key
) {
    for (const auto& field : event.fields) {
        if (field.key != key) {
            continue;
        }

        if (
            const auto* value =
                std::get_if<std::uint64_t>(
                    &field.value
                )
        ) {
            return *value;
        }

        if (
            const auto* value =
                std::get_if<std::int64_t>(
                    &field.value
                )
        ) {
            if (*value >= 0) {
                return
                    static_cast<std::uint64_t>(
                        *value
                    );
            }
        }
    }

    return std::nullopt;
}


std::optional<bool> boolField(
    const CommandEvent& event,
    const std::string& key
) {
    for (const auto& field : event.fields) {
        if (field.key != key) {
            continue;
        }

        if (
            const auto* value =
                std::get_if<bool>(
                    &field.value
                )
        ) {
            return *value;
        }
    }

    return std::nullopt;
}


std::optional<CommandEvent> lastEvent(
    const EventCapture& capture,
    const std::string& type
) {
    const auto events =
        capture.snapshot();

    for (
        auto it = events.rbegin();
        it != events.rend();
        ++it
    ) {
        if (it->type == type) {
            return *it;
        }
    }

    return std::nullopt;
}


std::size_t countEvents(
    const EventCapture& capture,
    const std::string& type
) {
    const auto events =
        capture.snapshot();

    std::size_t count = 0;

    for (const auto& event : events) {
        if (event.type == type) {
            ++count;
        }
    }

    return count;
}


std::optional<std::filesystem::path>
findOutputFile(
    const EventCapture& capture,
    const std::string& output_type
) {
    const auto events =
        capture.snapshot();

    for (const auto& event : events) {
        if (event.type != "output") {
            continue;
        }

        const auto type =
            stringField(
                event,
                "type"
            );

        const auto path =
            stringField(
                event,
                "path"
            );

        if (
            type &&
            path &&
            *type == output_type
        ) {
            return
                std::filesystem::path(
                    *path
                );
        }
    }

    return std::nullopt;
}


/*
 * ============================================================
 * Environment
 * ============================================================
 */

std::uint64_t readUnsignedEnv(
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
            << "Invalid "
            << name
            << ": "
            << value
            << '\n';

        std::exit(2);
    }
}


/*
 * ============================================================
 * Command invocation
 * ============================================================
 */

template<typename Function>
int runPhase(
    const std::string& name,
    Function&& function
) {
    std::cout
        << "\n--- "
        << name
        << " ---\n";

    try {
        const int result =
            function();

        if (result == 0) {
            pass(
                name +
                " returned 0"
            );
        }
        else {
            fail(
                name +
                " returned " +
                std::to_string(result)
            );
        }

        return result;
    }
    catch (const std::exception& error) {
        fail(
            name +
            " threw exception: " +
            error.what()
        );

        return -1;
    }
    catch (...) {
        fail(
            name +
            " threw unknown exception"
        );

        return -1;
    }
}


/*
 * After record-reset the USB CDC device may disappear and return.
 *
 * Instead of checking whether a /dev path exists, perform a real
 * identify command. This also works if the test is eventually run
 * on Windows.
 */
bool waitForDevice(
    const std::string& port,
    std::chrono::seconds timeout
) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        timeout;

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        EventCapture capture;

        CommandOutput output(
            "identify",
            capture.sink()
        );

        try {
            if (
                headmotion::app::
                    runIdentifyCommand(
                        port,
                        output
                    ) ==
                0
            ) {
                return true;
            }
        }
        catch (...) {
            /*
             * Expected while USB is still re-enumerating.
             */
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                500
            )
        );
    }

    return false;
}


/*
 * ============================================================
 * Temporary output
 * ============================================================
 */

std::filesystem::path makeOutputDirectory()
{
    const auto stamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const auto path =
        std::filesystem::temp_directory_path() /
        (
            "headmotion_hw_e2e_" +
            std::to_string(stamp)
        );

    std::filesystem::create_directories(
        path
    );

    return path;
}


/*
 * ============================================================
 * CSV validation
 * ============================================================
 */

std::uint64_t validateSimpleCsv(
    const std::filesystem::path& path,
    const std::string& expected_header
) {
    std::ifstream file(path);

    if (!file.is_open()) {
        fail(
            "could not open CSV: " +
            path.string()
        );

        return 0;
    }


    std::string header;

    if (!std::getline(file, header)) {
        fail(
            "CSV has no header: " +
            path.string()
        );

        return 0;
    }


    if (header != expected_header) {
        fail(
            "unexpected CSV header in " +
            path.string()
        );

        std::cerr
            << "      expected: "
            << expected_header
            << '\n'
            << "      actual:   "
            << header
            << '\n';
    }


    std::uint64_t rows = 0;
    std::string line;

    while (std::getline(file, line)) {
        if (!line.empty()) {
            ++rows;
        }
    }

    return rows;
}


std::uint64_t validateXsensCsv(
    const std::filesystem::path& path
) {
    std::ifstream file(path);

    if (!file.is_open()) {
        fail(
            "could not open Xsens CSV: " +
            path.string()
        );

        return 0;
    }


    std::string header;

    if (!std::getline(file, header)) {
        fail(
            "Xsens CSV has no header"
        );

        return 0;
    }


    if (header != XSENS_HEADER) {
        fail(
            "Xsens CSV header does not match expected format"
        );

        std::cerr
            << "      expected: "
            << XSENS_HEADER
            << '\n'
            << "      actual:   "
            << header
            << '\n';
    }


    std::uint64_t expected_packet =
        0;

    std::uint64_t rows =
        0;

    std::string line;


    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }


        /*
         * 13 columns means 12 commas.
         */
        std::size_t comma_count =
            0;

        for (const char ch : line) {
            if (ch == ',') {
                ++comma_count;
            }
        }


        if (comma_count != 12) {
            fail(
                "Xsens row " +
                std::to_string(rows) +
                " has " +
                std::to_string(
                    comma_count + 1
                ) +
                " columns instead of 13"
            );

            break;
        }


        const std::size_t comma =
            line.find(',');

        if (comma == std::string::npos) {
            fail(
                "Xsens row has no packet counter delimiter"
            );

            break;
        }


        try {
            const std::uint64_t packet =
                std::stoull(
                    line.substr(
                        0,
                        comma
                    )
                );

            if (
                packet !=
                expected_packet
            ) {
                fail(
                    "Xsens packet counter discontinuity: expected " +
                    std::to_string(
                        expected_packet
                    ) +
                    ", got " +
                    std::to_string(
                        packet
                    )
                );

                break;
            }
        }
        catch (...) {
            fail(
                "invalid Xsens packet counter at row " +
                std::to_string(rows)
            );

            break;
        }


        ++expected_packet;
        ++rows;
    }


    return rows;
}


/*
 * ============================================================
 * Main
 * ============================================================
 */

} // namespace


int main()
{
    const char* port_env =
        std::getenv(
            "HEADMOTION_HW_E2E_PORT"
        );


    /*
     * Hardware tests must never accidentally run just because
     * HEADMOTION_BUILD_TESTS is enabled.
     */
    if (
        port_env == nullptr ||
        *port_env == '\0'
    ) {
        std::cout
            << "SKIP: HEADMOTION_HW_E2E_PORT is not set\n"
            << "This is a destructive real-hardware test.\n";

        return SKIP_RETURN_CODE;
    }


    const std::string port =
        port_env;

    const std::uint64_t record_seconds =
        readUnsignedEnv(
            "HEADMOTION_HW_E2E_SECONDS",
            DEFAULT_RECORD_SECONDS
        );

    const std::uint64_t battery_interval =
        readUnsignedEnv(
            "HEADMOTION_HW_E2E_BATTERY_INTERVAL",
            DEFAULT_BATTERY_INTERVAL
        );


    if (record_seconds == 0) {
        std::cerr
            << "HEADMOTION_HW_E2E_SECONDS must be > 0\n";

        return 2;
    }


    const std::filesystem::path output_dir =
        makeOutputDirectory();


    std::cout
        << "========================================\n"
        << "HeadMotion MMS+ Hardware E2E Test\n"
        << "========================================\n\n"
        << "WARNING: THIS TEST IS DESTRUCTIVE\n"
        << "Existing MMS+ recordings will be erased.\n\n"
        << "Port:               "
        << port
        << '\n'
        << "Sample rate:        "
        << SAMPLE_RATE_HZ
        << " Hz\n"
        << "Record duration:    "
        << record_seconds
        << " seconds\n"
        << "Battery interval:   "
        << battery_interval
        << " seconds\n"
        << "Output directory:   "
        << output_dir
        << "\n";


    bool device_was_identified =
        false;

    bool recording_started =
        false;

    bool recording_stopped =
        false;

    bool sync_completed =
        false;


    /*
     * ========================================================
     * 1. Initial identify
     * ========================================================
     */

    EventCapture identify_capture;

    CommandOutput identify_output(
        "identify",
        identify_capture.sink()
    );


    if (
        runPhase(
            "initial identify",
            [&] {
                return
                    headmotion::app::
                        runIdentifyCommand(
                            port,
                            identify_output
                        );
            }
        ) ==
        0
    ) {
        const auto identity_event =
            lastEvent(
                identify_capture,
                "identity"
            );


        if (!identity_event) {
            fail(
                "identify returned success but emitted no identity event"
            );
        }
        else {
            const auto identity =
                stringField(
                    *identity_event,
                    "identity"
                );


            if (
                !identity ||
                identity->empty()
            ) {
                fail(
                    "identity event contains an empty identity string"
                );
            }
            else {
                pass(
                    "MMS+ identity response: " +
                    *identity
                );

                device_was_identified =
                    true;
            }
        }
    }


    /*
     * ========================================================
     * 2. Initial destructive reset
     * ========================================================
     */

    bool ready_after_reset =
        false;


    if (device_was_identified) {
        EventCapture reset_capture;

        CommandOutput reset_output(
            "record-reset",
            reset_capture.sink()
        );


        if (
            runPhase(
                "initial record-reset",
                [&] {
                    return
                        headmotion::app::
                            runRecordResetCommand(
                                port,
                                reset_output
                            );
                }
            ) ==
            0
        ) {
            std::cout
                << "Waiting for USB re-enumeration...\n";


            ready_after_reset =
                waitForDevice(
                    port,
                    std::chrono::seconds(
                        20
                    )
                );


            if (ready_after_reset) {
                pass(
                    "device returned after initial reset"
                );
            }
            else {
                fail(
                    "device did not become responsive after initial reset"
                );
            }
        }
    }


    /*
     * ========================================================
     * 3. Start recording
     * ========================================================
     */

    EventCapture start_capture;


    if (ready_after_reset) {
        CommandOutput start_output(
            "record-start",
            start_capture.sink()
        );


        if (
            runPhase(
                "record-start",
                [&] {
                    return
                        headmotion::app::
                            runRecordStartCommand(
                                port,
                                SAMPLE_RATE_HZ,
                                static_cast<std::uint32_t>(
                                    battery_interval
                                ),
                                start_output
                            );
                }
            ) ==
            0
        ) {
            recording_started =
                true;


            const std::size_t logger_count =
                countEvents(
                    start_capture,
                    "logger_created"
                );


            if (logger_count < 2) {
                fail(
                    "record-start created fewer than 2 sensor loggers"
                );
            }
            else {
                pass(
                    "accelerometer and gyro loggers were created"
                );
            }


            const auto summary =
                lastEvent(
                    start_capture,
                    "summary"
                );


            if (!summary) {
                fail(
                    "record-start emitted no summary event"
                );
            }
            else {
                const auto recording =
                    boolField(
                        *summary,
                        "recording"
                    );


                if (
                    !recording ||
                    !*recording
                ) {
                    fail(
                        "record-start summary did not report recording=true"
                    );
                }
                else {
                    pass(
                        "record-start reported recording=true"
                    );
                }
            }
        }
    }


    /*
     * ========================================================
     * 4. Actually collect samples
     * ========================================================
     */

    if (recording_started) {
        std::cout
            << "\nRecording real MMS+ data for "
            << record_seconds
            << " seconds...\n";


        std::this_thread::sleep_for(
            std::chrono::seconds(
                record_seconds
            )
        );


        pass(
            "recording interval completed"
        );
    }


    /*
     * ========================================================
     * 5. Stop recording
     * ========================================================
     */

    if (recording_started) {
        EventCapture stop_capture;

        CommandOutput stop_output(
            "record-stop",
            stop_capture.sink()
        );


        if (
            runPhase(
                "record-stop",
                [&] {
                    return
                        headmotion::app::
                            runRecordStopCommand(
                                port,
                                stop_output
                            );
                }
            ) ==
            0
        ) {
            recording_stopped =
                true;


            const auto summary =
                lastEvent(
                    stop_capture,
                    "summary"
                );


            if (!summary) {
                fail(
                    "record-stop emitted no summary event"
                );
            }
            else {
                const auto stopped =
                    boolField(
                        *summary,
                        "logging_stopped"
                    );


                if (
                    !stopped ||
                    !*stopped
                ) {
                    fail(
                        "record-stop summary did not report logging_stopped=true"
                    );
                }
                else {
                    pass(
                        "record-stop reported logging_stopped=true"
                    );
                }
            }
        }
    }


    /*
     * ========================================================
     * 6. Download real logged data
     * ========================================================
     */

    EventCapture sync_capture;


    if (recording_stopped) {
        CommandOutput sync_output(
            "sync",
            sync_capture.sink()
        );


        if (
            runPhase(
                "sync",
                [&] {
                    /*
                     * true = also write legacy long-format IMU CSV.
                     *
                     * For a short hardware integration test this gives
                     * us another real output path to validate.
                     */
                    return
                        headmotion::app::
                            runSyncCommand(
                                port,
                                output_dir.string(),
                                true,
                                sync_output
                            );
                }
            ) ==
            0
        ) {
            sync_completed =
                true;
        }
    }


    /*
     * ========================================================
     * 7. Validate sync summary
     * ========================================================
     */

    if (sync_completed) {
        const auto summary =
            lastEvent(
                sync_capture,
                "summary"
            );


        if (!summary) {
            fail(
                "sync emitted no summary event"
            );
        }
        else {
            const auto imu_samples =
                uintField(
                    *summary,
                    "imu_samples_received"
                );

            const auto xsens_rows =
                uintField(
                    *summary,
                    "xsens_rows_written"
                );

            const auto legacy_rows =
                uintField(
                    *summary,
                    "legacy_imu_rows_written"
                );


            if (!imu_samples) {
                fail(
                    "sync summary missing imu_samples_received"
                );
            }
            else if (*imu_samples == 0) {
                fail(
                    "sync received zero IMU samples"
                );
            }
            else {
                pass(
                    "sync received " +
                    std::to_string(
                        *imu_samples
                    ) +
                    " IMU samples"
                );
            }


            if (!xsens_rows) {
                fail(
                    "sync summary missing xsens_rows_written"
                );
            }
            else if (*xsens_rows == 0) {
                fail(
                    "sync produced zero paired Xsens rows"
                );
            }
            else {
                pass(
                    "sync produced " +
                    std::to_string(
                        *xsens_rows
                    ) +
                    " paired Xsens rows"
                );
            }


            if (
                imu_samples &&
                xsens_rows &&
                *imu_samples <
                    (*xsens_rows * 2)
            ) {
                fail(
                    "paired row count exceeds available accel+gyro samples"
                );
            }


            if (
                legacy_rows &&
                imu_samples &&
                *legacy_rows !=
                    *imu_samples
            ) {
                fail(
                    "legacy IMU row count does not match received IMU sample count"
                );
            }
        }
    }


    /*
     * ========================================================
     * 8. Validate Xsens CSV
     * ========================================================
     */

    if (sync_completed) {
        const auto xsens_path =
            findOutputFile(
                sync_capture,
                "xsens_csv"
            );


        if (!xsens_path) {
            fail(
                "sync emitted no xsens_csv output path"
            );
        }
        else {
            const std::uint64_t csv_rows =
                validateXsensCsv(
                    *xsens_path
                );


            const std::uint64_t
                approximate_expected =
                    static_cast<std::uint64_t>(
                        SAMPLE_RATE_HZ *
                        static_cast<float>(
                            record_seconds
                        )
                    );


            /*
             * We're testing the full lifecycle, not doing a precise
             * ODR calibration here.
             *
             * record-stop itself takes time while the logger is still
             * alive, so the upper count may legitimately exceed the
             * requested sleep interval.
             *
             * Requiring at least half of the nominal samples catches
             * obviously broken recording without making this test flaky.
             */
            const std::uint64_t minimum_rows =
                approximate_expected /
                2;


            if (
                csv_rows <
                minimum_rows
            ) {
                fail(
                    "Xsens CSV contains only " +
                    std::to_string(
                        csv_rows
                    ) +
                    " rows; expected at least " +
                    std::to_string(
                        minimum_rows
                    )
                );
            }
            else {
                pass(
                    "Xsens CSV contains " +
                    std::to_string(
                        csv_rows
                    ) +
                    " valid contiguous rows"
                );
            }


            const auto summary =
                lastEvent(
                    sync_capture,
                    "summary"
                );


            if (summary) {
                const auto reported_rows =
                    uintField(
                        *summary,
                        "xsens_rows_written"
                    );


                if (
                    reported_rows &&
                    *reported_rows !=
                        csv_rows
                ) {
                    fail(
                        "Xsens file row count does not match sync summary"
                    );
                }
                else if (reported_rows) {
                    pass(
                        "Xsens file row count matches sync summary"
                    );
                }
            }
        }
    }


    /*
     * ========================================================
     * 9. Validate legacy IMU CSV
     * ========================================================
     */

    if (sync_completed) {
        const auto legacy_path =
            findOutputFile(
                sync_capture,
                "legacy_imu_csv"
            );


        if (!legacy_path) {
            fail(
                "sync emitted no legacy_imu_csv output path"
            );
        }
        else {
            const std::uint64_t rows =
                validateSimpleCsv(
                    *legacy_path,
                    LEGACY_HEADER
                );


            const auto summary =
                lastEvent(
                    sync_capture,
                    "summary"
                );


            if (summary) {
                const auto reported_rows =
                    uintField(
                        *summary,
                        "legacy_imu_rows_written"
                    );


                if (
                    reported_rows &&
                    *reported_rows != rows
                ) {
                    fail(
                        "legacy CSV row count does not match sync summary"
                    );
                }
                else if (reported_rows) {
                    pass(
                        "legacy CSV contains " +
                        std::to_string(
                            rows
                        ) +
                        " rows"
                    );
                }
            }
        }
    }


    /*
     * ========================================================
     * 10. Optional battery CSV validation
     * ========================================================
     */

    if (
        sync_completed &&
        battery_interval > 0
    ) {
        const auto battery_path =
            findOutputFile(
                sync_capture,
                "battery_csv"
            );


        if (!battery_path) {
            fail(
                "battery logging was enabled but no battery CSV was emitted"
            );
        }
        else {
            const std::uint64_t rows =
                validateSimpleCsv(
                    *battery_path,
                    BATTERY_HEADER
                );


            /*
             * Only require a sample if the requested record interval
             * was at least as long as the battery timer interval.
             */
            if (
                record_seconds >=
                    battery_interval &&
                rows == 0
            ) {
                fail(
                    "battery CSV contains no samples"
                );
            }
            else {
                pass(
                    "battery CSV validated"
                );
            }
        }
    }


    /*
     * ========================================================
     * 11. Emergency stop if normal stop failed
     * ========================================================
     */

    if (
        recording_started &&
        !recording_stopped
    ) {
        std::cerr
            << "\nAttempting emergency record-stop...\n";


        try {
            EventCapture emergency_capture;

            CommandOutput emergency_output(
                "record-stop",
                emergency_capture.sink()
            );


            headmotion::app::
                runRecordStopCommand(
                    port,
                    emergency_output
                );
        }
        catch (...) {
            std::cerr
                << "WARNING: emergency record-stop failed\n";
        }
    }


    /*
     * ========================================================
     * 12. Final board cleanup
     * ========================================================
     */

    if (device_was_identified) {
        /*
         * If the device is temporarily unavailable, give it a chance
         * to return before cleanup.
         */
        waitForDevice(
            port,
            std::chrono::seconds(
                10
            )
        );


        EventCapture final_reset_capture;

        CommandOutput final_reset_output(
            "record-reset",
            final_reset_capture.sink()
        );


        const int reset_result =
            runPhase(
                "final record-reset",
                [&] {
                    return
                        headmotion::app::
                            runRecordResetCommand(
                                port,
                                final_reset_output
                            );
                }
            );


        if (reset_result == 0) {
            if (
                waitForDevice(
                    port,
                    std::chrono::seconds(
                        20
                    )
                )
            ) {
                pass(
                    "device returned after final reset"
                );
            }
            else {
                fail(
                    "device did not become responsive after final reset"
                );
            }
        }
    }


    /*
     * ========================================================
     * Final result
     * ========================================================
     */

    std::cout
        << "\n========================================\n"
        << "Hardware E2E Result\n"
        << "========================================\n";


    if (failures != 0) {
        std::cerr
            << failures
            << " hardware integration assertion(s) FAILED\n"
            << "\nCSV artifacts preserved at:\n  "
            << output_dir
            << '\n';

        return 1;
    }


    std::error_code error;

    std::filesystem::remove_all(
        output_dir,
        error
    );


    std::cout
        << "PASS: identify\n"
        << "PASS: initial board reset\n"
        << "PASS: 50 Hz logger creation\n"
        << "PASS: real onboard recording\n"
        << "PASS: record-stop\n"
        << "PASS: real log download\n"
        << "PASS: Xsens CSV integrity\n"
        << "PASS: legacy IMU CSV integrity\n"
        << "PASS: final board reset\n\n"
        << "HARDWARE END-TO-END TEST PASSED\n";


    return 0;
}