/*
 * Multi-device MMS+ hardware end-to-end integration test.
 *
 * IMPORTANT:
 *   Devices are tested SEQUENTIALLY, not concurrently.
 *
 * REQUIRES 2+ REAL MMS+ DEVICES.
 *
 * THIS TEST IS DESTRUCTIVE:
 *   - clears existing recordings
 *   - creates new logger routes
 *   - records real accel/gyro data
 *   - downloads real logs
 *   - resets every device afterward
 *
 * Required environment variable:
 *
 *   HEADMOTION_HW_E2E_PORTS
 *
 * Example:
 *
 *   HEADMOTION_HW_E2E_PORTS="\
 * /dev/serial/by-id/device1,\
 * /dev/serial/by-id/device2" \
 * ./build/linux-native-debug/headmotion_test_hardware_multi_device_e2e
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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
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


/*
 * ============================================================
 * Device state
 * ============================================================
 */

struct DeviceState {
    std::size_t index = 0;

    std::string port;

    std::string identity;
    std::string device_id;

    bool identified = false;
    bool initial_reset_completed = false;

    bool recording_started = false;
    bool recording_stopped = false;

    bool sync_completed = false;
    bool final_reset_completed = false;

    std::uint64_t imu_samples = 0;
    std::uint64_t xsens_rows = 0;
    std::uint64_t legacy_rows = 0;
    std::uint64_t battery_rows = 0;

    std::optional<std::filesystem::path>
        xsens_path;

    std::optional<std::filesystem::path>
        legacy_path;

    std::optional<std::filesystem::path>
        battery_path;

    std::vector<std::string> errors;
};


/*
 * ============================================================
 * Reporting
 * ============================================================
 */

void addFailure(
    DeviceState& device,
    const std::string& message
) {
    device.errors.push_back(
        message
    );

    std::cerr
        << "[device "
        << device.index
        << "] FAIL: "
        << message
        << '\n';
}


void addPass(
    const DeviceState& device,
    const std::string& message
) {
    std::cout
        << "[device "
        << device.index
        << "] PASS: "
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
 * Environment parsing
 * ============================================================
 */

std::string trim(
    std::string value
) {
    while (
        !value.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                value.front()
            )
        )
    ) {
        value.erase(
            value.begin()
        );
    }

    while (
        !value.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                value.back()
            )
        )
    ) {
        value.pop_back();
    }

    return value;
}


std::vector<std::string> parsePorts(
    const std::string& value
) {
    std::vector<std::string> ports;

    std::string current;

    for (const char ch : value) {
        if (
            ch == ',' ||
            ch == ';'
        ) {
            current =
                trim(current);

            if (!current.empty()) {
                ports.push_back(
                    current
                );
            }

            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    current =
        trim(current);

    if (!current.empty()) {
        ports.push_back(
            current
        );
    }

    return ports;
}


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
        return
            std::stoull(
                value
            );
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
 * Device readiness
 * ============================================================
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
             * Expected while USB is re-enumerating.
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
            "headmotion_multi_hw_e2e_" +
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
    DeviceState& device,
    const std::filesystem::path& path,
    const std::string& expected_header
) {
    std::ifstream file(path);

    if (!file.is_open()) {
        addFailure(
            device,
            "could not open CSV: " +
            path.string()
        );

        return 0;
    }


    std::string header;

    if (!std::getline(file, header)) {
        addFailure(
            device,
            "CSV has no header: " +
            path.string()
        );

        return 0;
    }


    if (header != expected_header) {
        addFailure(
            device,
            "unexpected CSV header: " +
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
    DeviceState& device,
    const std::filesystem::path& path
) {
    std::ifstream file(path);

    if (!file.is_open()) {
        addFailure(
            device,
            "could not open Xsens CSV: " +
            path.string()
        );

        return 0;
    }


    std::string header;

    if (!std::getline(file, header)) {
        addFailure(
            device,
            "Xsens CSV has no header"
        );

        return 0;
    }


    if (header != XSENS_HEADER) {
        addFailure(
            device,
            "Xsens CSV header mismatch"
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
         * Xsens output has 13 columns = 12 commas.
         */
        std::size_t comma_count =
            0;

        for (const char ch : line) {
            if (ch == ',') {
                ++comma_count;
            }
        }


        if (comma_count != 12) {
            addFailure(
                device,
                "Xsens row " +
                std::to_string(rows) +
                " contains " +
                std::to_string(
                    comma_count + 1
                ) +
                " columns instead of 13"
            );

            break;
        }


        const std::size_t first_comma =
            line.find(',');


        if (
            first_comma ==
            std::string::npos
        ) {
            addFailure(
                device,
                "Xsens row missing packet counter delimiter"
            );

            break;
        }


        try {
            const std::uint64_t packet =
                std::stoull(
                    line.substr(
                        0,
                        first_comma
                    )
                );


            if (
                packet !=
                expected_packet
            ) {
                addFailure(
                    device,
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
            addFailure(
                device,
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
 * Cleanup
 * ============================================================
 */

void attemptFinalCleanup(
    DeviceState& device
) {
    if (!device.identified) {
        return;
    }


    /*
     * If record-stop failed, make a best-effort stop before reset.
     */
    if (
        device.recording_started &&
        !device.recording_stopped
    ) {
        std::cerr
            << "[device "
            << device.index
            << "] attempting emergency record-stop\n";


        try {
            EventCapture stop_capture;

            CommandOutput stop_output(
                "record-stop",
                stop_capture.sink()
            );


            headmotion::app::
                runRecordStopCommand(
                    device.port,
                    stop_output
                );
        }
        catch (const std::exception& error) {
            std::cerr
                << "[device "
                << device.index
                << "] WARNING: emergency record-stop failed: "
                << error.what()
                << '\n';
        }
        catch (...) {
            std::cerr
                << "[device "
                << device.index
                << "] WARNING: emergency record-stop failed\n";
        }
    }


    /*
     * Give the USB device a chance to be reachable.
     */
    waitForDevice(
        device.port,
        std::chrono::seconds(
            10
        )
    );


    try {
        EventCapture reset_capture;

        CommandOutput reset_output(
            "record-reset",
            reset_capture.sink()
        );


        const int result =
            headmotion::app::
                runRecordResetCommand(
                    device.port,
                    reset_output
                );


        if (result != 0) {
            addFailure(
                device,
                "final record-reset returned " +
                std::to_string(result)
            );

            return;
        }


        if (
            !waitForDevice(
                device.port,
                std::chrono::seconds(
                    20
                )
            )
        ) {
            addFailure(
                device,
                "device did not return after final reset"
            );

            return;
        }


        device.final_reset_completed =
            true;


        addPass(
            device,
            "final reset completed"
        );
    }
    catch (const std::exception& error) {
        addFailure(
            device,
            std::string(
                "final cleanup threw exception: "
            ) +
            error.what()
        );
    }
    catch (...) {
        addFailure(
            device,
            "final cleanup threw unknown exception"
        );
    }
}


/*
 * ============================================================
 * Complete sequential lifecycle for ONE device
 * ============================================================
 */

void runDeviceLifecycle(
    DeviceState& device,
    const std::filesystem::path& output_dir,
    std::uint64_t record_seconds,
    std::uint64_t battery_interval
) {
    std::cout
        << "\n"
        << "============================================================\n"
        << "DEVICE "
        << device.index
        << "\n"
        << device.port
        << "\n"
        << "============================================================\n";


    /*
     * --------------------------------------------------------
     * 1. Identify
     * --------------------------------------------------------
     */

    try {
        EventCapture capture;

        CommandOutput output(
            "identify",
            capture.sink()
        );


        const int result =
            headmotion::app::
                runIdentifyCommand(
                    device.port,
                    output
                );


        if (result != 0) {
            addFailure(
                device,
                "identify returned " +
                std::to_string(result)
            );

            return;
        }


        const auto event =
            lastEvent(
                capture,
                "identity"
            );


        if (!event) {
            addFailure(
                device,
                "identify emitted no identity event"
            );

            return;
        }


        const auto identity =
            stringField(
                *event,
                "identity"
            );


        if (
            !identity ||
            identity->empty()
        ) {
            addFailure(
                device,
                "identify returned empty identity"
            );

            return;
        }


        device.identity =
            *identity;

        device.identified =
            true;


        addPass(
            device,
            "identity = " +
            device.identity
        );
    }
    catch (const std::exception& error) {
        addFailure(
            device,
            std::string(
                "identify threw exception: "
            ) +
            error.what()
        );

        return;
    }


    /*
     * Once the device has been identified, always attempt final cleanup
     * before leaving this function.
     */
    const auto finish =
        [&device]() {
            attemptFinalCleanup(
                device
            );
        };


    /*
     * --------------------------------------------------------
     * 2. Initial reset
     * --------------------------------------------------------
     */

    try {
        EventCapture capture;

        CommandOutput output(
            "record-reset",
            capture.sink()
        );


        const int result =
            headmotion::app::
                runRecordResetCommand(
                    device.port,
                    output
                );


        if (result != 0) {
            addFailure(
                device,
                "initial record-reset returned " +
                std::to_string(result)
            );

            finish();
            return;
        }


        if (
            !waitForDevice(
                device.port,
                std::chrono::seconds(
                    20
                )
            )
        ) {
            addFailure(
                device,
                "device did not return after initial reset"
            );

            finish();
            return;
        }


        device.initial_reset_completed =
            true;


        addPass(
            device,
            "initial reset completed"
        );
    }
    catch (const std::exception& error) {
        addFailure(
            device,
            std::string(
                "initial reset threw exception: "
            ) +
            error.what()
        );

        finish();
        return;
    }


    /*
     * --------------------------------------------------------
     * 3. Start recording
     * --------------------------------------------------------
     */

    try {
        EventCapture capture;

        CommandOutput output(
            "record-start",
            capture.sink()
        );


        const int result =
            headmotion::app::
                runRecordStartCommand(
                    device.port,
                    SAMPLE_RATE_HZ,
                    static_cast<std::uint32_t>(
                        battery_interval
                    ),
                    output
                );


        if (result != 0) {
            addFailure(
                device,
                "record-start returned " +
                std::to_string(result)
            );

            finish();
            return;
        }


        device.recording_started =
            true;


        const std::size_t expected_loggers =
            battery_interval > 0
                ? 3
                : 2;


        const std::size_t logger_count =
            countEvents(
                capture,
                "logger_created"
            );


        if (
            logger_count <
            expected_loggers
        ) {
            addFailure(
                device,
                "expected at least " +
                std::to_string(
                    expected_loggers
                ) +
                " logger_created events, got " +
                std::to_string(
                    logger_count
                )
            );
        }
        else {
            addPass(
                device,
                "logger routes created"
            );
        }


        const auto summary =
            lastEvent(
                capture,
                "summary"
            );


        if (!summary) {
            addFailure(
                device,
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
                addFailure(
                    device,
                    "record-start did not report recording=true"
                );
            }
            else {
                addPass(
                    device,
                    "recording=true"
                );
            }
        }
    }
    catch (const std::exception& error) {
        addFailure(
            device,
            std::string(
                "record-start threw exception: "
            ) +
            error.what()
        );

        finish();
        return;
    }


    /*
     * --------------------------------------------------------
     * 4. Real recording interval
     * --------------------------------------------------------
     */

    std::cout
        << "[device "
        << device.index
        << "] recording real data for "
        << record_seconds
        << " second(s)...\n";


    std::this_thread::sleep_for(
        std::chrono::seconds(
            record_seconds
        )
    );


    addPass(
        device,
        "recording interval completed"
    );


    /*
     * --------------------------------------------------------
     * 5. Stop recording
     * --------------------------------------------------------
     */

    try {
        EventCapture capture;

        CommandOutput output(
            "record-stop",
            capture.sink()
        );


        const int result =
            headmotion::app::
                runRecordStopCommand(
                    device.port,
                    output
                );


        if (result != 0) {
            addFailure(
                device,
                "record-stop returned " +
                std::to_string(result)
            );

            finish();
            return;
        }


        device.recording_stopped =
            true;


        const auto summary =
            lastEvent(
                capture,
                "summary"
            );


        if (!summary) {
            addFailure(
                device,
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
                addFailure(
                    device,
                    "record-stop did not report logging_stopped=true"
                );
            }
            else {
                addPass(
                    device,
                    "logging stopped"
                );
            }
        }
    }
    catch (const std::exception& error) {
        addFailure(
            device,
            std::string(
                "record-stop threw exception: "
            ) +
            error.what()
        );

        finish();
        return;
    }


    /*
     * --------------------------------------------------------
     * 6. Sync/download
     * --------------------------------------------------------
     */

    EventCapture sync_capture;


    try {
        CommandOutput output(
            "sync",
            sync_capture.sink()
        );


        /*
         * true = also write legacy long-format IMU CSV.
         */
        const int result =
            headmotion::app::
                runSyncCommand(
                    device.port,
                    output_dir.string(),
                    true,
                    output
                );


        if (result != 0) {
            addFailure(
                device,
                "sync returned " +
                std::to_string(result)
            );

            finish();
            return;
        }


        device.sync_completed =
            true;


        addPass(
            device,
            "sync completed"
        );
    }
    catch (const std::exception& error) {
        addFailure(
            device,
            std::string(
                "sync threw exception: "
            ) +
            error.what()
        );

        finish();
        return;
    }


    /*
     * --------------------------------------------------------
     * 7. Parse sync device ID
     * --------------------------------------------------------
     */

    {
        const auto event =
            lastEvent(
                sync_capture,
                "device"
            );


        if (!event) {
            addFailure(
                device,
                "sync emitted no device event"
            );
        }
        else {
            const auto id =
                stringField(
                    *event,
                    "device_id"
                );


            if (
                !id ||
                id->empty()
            ) {
                addFailure(
                    device,
                    "sync emitted empty device ID"
                );
            }
            else {
                device.device_id =
                    *id;


                addPass(
                    device,
                    "sync device ID = " +
                    device.device_id
                );
            }
        }
    }


    /*
     * --------------------------------------------------------
     * 8. Parse sync summary
     * --------------------------------------------------------
     */

    {
        const auto summary =
            lastEvent(
                sync_capture,
                "summary"
            );


        if (!summary) {
            addFailure(
                device,
                "sync emitted no summary event"
            );
        }
        else {
            if (
                const auto value =
                    uintField(
                        *summary,
                        "imu_samples_received"
                    )
            ) {
                device.imu_samples =
                    *value;
            }


            if (
                const auto value =
                    uintField(
                        *summary,
                        "xsens_rows_written"
                    )
            ) {
                device.xsens_rows =
                    *value;
            }


            if (
                const auto value =
                    uintField(
                        *summary,
                        "legacy_imu_rows_written"
                    )
            ) {
                device.legacy_rows =
                    *value;
            }


            if (
                const auto value =
                    uintField(
                        *summary,
                        "battery_rows_written"
                    )
            ) {
                device.battery_rows =
                    *value;
            }
        }


        if (device.imu_samples == 0) {
            addFailure(
                device,
                "sync received zero IMU samples"
            );
        }
        else {
            addPass(
                device,
                "received " +
                std::to_string(
                    device.imu_samples
                ) +
                " IMU samples"
            );
        }


        if (device.xsens_rows == 0) {
            addFailure(
                device,
                "sync wrote zero paired Xsens rows"
            );
        }
        else {
            addPass(
                device,
                "sync wrote " +
                std::to_string(
                    device.xsens_rows
                ) +
                " paired Xsens rows"
            );
        }


        if (
            device.legacy_rows !=
            device.imu_samples
        ) {
            addFailure(
                device,
                "legacy IMU row count does not match "
                "received IMU sample count"
            );
        }
    }


    /*
     * --------------------------------------------------------
     * 9. Resolve generated CSVs
     * --------------------------------------------------------
     */

    device.xsens_path =
        findOutputFile(
            sync_capture,
            "xsens_csv"
        );

    device.legacy_path =
        findOutputFile(
            sync_capture,
            "legacy_imu_csv"
        );


    if (battery_interval > 0) {
        device.battery_path =
            findOutputFile(
                sync_capture,
                "battery_csv"
            );
    }


    /*
     * --------------------------------------------------------
     * 10. Validate Xsens CSV
     * --------------------------------------------------------
     */

    if (!device.xsens_path) {
        addFailure(
            device,
            "sync emitted no xsens_csv output path"
        );
    }
    else {
        const std::uint64_t actual_rows =
            validateXsensCsv(
                device,
                *device.xsens_path
            );


        /*
         * This is an end-to-end functional test, not an exact
         * sample-rate calibration.
         *
         * Require at least half the nominal number of 50 Hz rows.
         */
        const std::uint64_t nominal_rows =
            static_cast<std::uint64_t>(
                SAMPLE_RATE_HZ *
                static_cast<float>(
                    record_seconds
                )
            );


        const std::uint64_t minimum_rows =
            nominal_rows /
            2;


        if (
            actual_rows <
            minimum_rows
        ) {
            addFailure(
                device,
                "Xsens CSV contains only " +
                std::to_string(
                    actual_rows
                ) +
                " rows; expected at least " +
                std::to_string(
                    minimum_rows
                )
            );
        }


        if (
            actual_rows !=
            device.xsens_rows
        ) {
            addFailure(
                device,
                "Xsens CSV row count differs from sync summary"
            );
        }
        else {
            addPass(
                device,
                "Xsens CSV validated with " +
                std::to_string(
                    actual_rows
                ) +
                " rows"
            );
        }
    }


    /*
     * --------------------------------------------------------
     * 11. Validate legacy IMU CSV
     * --------------------------------------------------------
     */

    if (!device.legacy_path) {
        addFailure(
            device,
            "sync emitted no legacy_imu_csv output path"
        );
    }
    else {
        const std::uint64_t actual_rows =
            validateSimpleCsv(
                device,
                *device.legacy_path,
                LEGACY_HEADER
            );


        if (
            actual_rows !=
            device.legacy_rows
        ) {
            addFailure(
                device,
                "legacy CSV row count differs from sync summary"
            );
        }
        else {
            addPass(
                device,
                "legacy CSV validated with " +
                std::to_string(
                    actual_rows
                ) +
                " rows"
            );
        }
    }


    /*
     * --------------------------------------------------------
     * 12. Optional battery CSV
     * --------------------------------------------------------
     */

    if (battery_interval > 0) {
        if (!device.battery_path) {
            addFailure(
                device,
                "battery logging enabled but no battery CSV emitted"
            );
        }
        else {
            const std::uint64_t actual_rows =
                validateSimpleCsv(
                    device,
                    *device.battery_path,
                    BATTERY_HEADER
                );


            if (
                record_seconds >=
                    battery_interval &&
                actual_rows == 0
            ) {
                addFailure(
                    device,
                    "battery CSV contains no samples"
                );
            }


            if (
                actual_rows !=
                device.battery_rows
            ) {
                addFailure(
                    device,
                    "battery CSV row count differs from sync summary"
                );
            }
            else {
                addPass(
                    device,
                    "battery CSV validated"
                );
            }
        }
    }


    /*
     * --------------------------------------------------------
     * 13. Final cleanup
     * --------------------------------------------------------
     */

    finish();
}


/*
 * ============================================================
 * Cross-device validation
 * ============================================================
 */

void validateAcrossDevices(
    std::vector<DeviceState>& devices
) {
    std::cout
        << "\n"
        << "============================================================\n"
        << "CROSS-DEVICE VALIDATION\n"
        << "============================================================\n";


    std::set<std::string>
        identities;

    std::set<std::string>
        device_ids;

    std::set<std::filesystem::path>
        output_paths;


    for (auto& device : devices) {
        /*
         * Identity strings returned by the physical devices should
         * distinguish the devices.
         */
        if (!device.identity.empty()) {
            if (
                !identities
                    .insert(
                        device.identity
                    )
                    .second
            ) {
                addFailure(
                    device,
                    "duplicate physical identity detected: " +
                    device.identity
                );
            }
        }


        if (!device.device_id.empty()) {
            if (
                !device_ids
                    .insert(
                        device.device_id
                    )
                    .second
            ) {
                addFailure(
                    device,
                    "duplicate output device ID detected: " +
                    device.device_id
                );
            }
        }


        const auto checkPath =
            [
                &device,
                &output_paths
            ](
                const std::optional<
                    std::filesystem::path
                >& path,
                const std::string& description
            ) {
                if (!path) {
                    return;
                }


                if (
                    !output_paths
                        .insert(*path)
                        .second
                ) {
                    addFailure(
                        device,
                        "duplicate " +
                        description +
                        " path: " +
                        path->string()
                    );
                }
            };


        checkPath(
            device.xsens_path,
            "Xsens CSV"
        );

        checkPath(
            device.legacy_path,
            "legacy CSV"
        );

        checkPath(
            device.battery_path,
            "battery CSV"
        );
    }


    if (
        device_ids.size() ==
        devices.size()
    ) {
        std::cout
            << "PASS: all devices resolved to unique output device IDs\n";
    }


    if (
        identities.size() ==
        devices.size()
    ) {
        std::cout
            << "PASS: all physical device identities are unique\n";
    }


    std::cout
        << "PASS: cross-device output collision check completed\n";
}


/*
 * ============================================================
 * Total failure count
 * ============================================================
 */

std::size_t totalFailures(
    const std::vector<DeviceState>& devices
) {
    std::size_t count = 0;


    for (const auto& device : devices) {
        count +=
            device.errors.size();
    }


    return count;
}

} // namespace


int main()
{
    /*
     * ========================================================
     * Configuration
     * ========================================================
     */

    const char* ports_env =
        std::getenv(
            "HEADMOTION_HW_E2E_PORTS"
        );


    if (
        ports_env == nullptr ||
        *ports_env == '\0'
    ) {
        std::cout
            << "SKIP: HEADMOTION_HW_E2E_PORTS is not set\n"
            << "This test requires at least two real MMS+ devices.\n";

        return SKIP_RETURN_CODE;
    }


    const std::vector<std::string> ports =
        parsePorts(
            ports_env
        );


    if (ports.size() < 2) {
        std::cout
            << "SKIP: multi-device E2E requires at least "
            << "two MMS+ ports\n";

        return SKIP_RETURN_CODE;
    }


    /*
     * Catch the obvious case where the exact same path is listed twice.
     */
    {
        const std::set<std::string> unique_ports(
            ports.begin(),
            ports.end()
        );


        if (
            unique_ports.size() !=
            ports.size()
        ) {
            std::cerr
                << "ERROR: duplicate port supplied in "
                << "HEADMOTION_HW_E2E_PORTS\n";

            return 2;
        }
    }


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


    std::vector<DeviceState> devices(
        ports.size()
    );


    for (
        std::size_t i = 0;
        i < ports.size();
        ++i
    ) {
        devices[i].index =
            i;

        devices[i].port =
            ports[i];
    }


    std::cout
        << "============================================================\n"
        << "HeadMotion Multi-Device Hardware E2E Test\n"
        << "============================================================\n\n"
        << "WARNING: THIS TEST IS DESTRUCTIVE\n"
        << "Existing recordings on every listed MMS+ will be erased.\n\n"
        << "Execution mode:     SEQUENTIAL\n"
        << "Devices:            "
        << devices.size()
        << '\n'
        << "Sample rate:        "
        << SAMPLE_RATE_HZ
        << " Hz\n"
        << "Record duration:    "
        << record_seconds
        << " second(s)/device\n"
        << "Battery interval:   "
        << battery_interval
        << " second(s)\n"
        << "Shared output dir:  "
        << output_dir
        << "\n\n";


    for (const auto& device : devices) {
        std::cout
            << "device "
            << device.index
            << ": "
            << device.port
            << '\n';
    }


    /*
     * ========================================================
     * Test each physical device ONE AT A TIME.
     * ========================================================
     */

    for (auto& device : devices) {
        runDeviceLifecycle(
            device,
            output_dir,
            record_seconds,
            battery_interval
        );
    }


    /*
     * ========================================================
     * Cross-device checks
     * ========================================================
     */

    validateAcrossDevices(
        devices
    );


    /*
     * ========================================================
     * Final report
     * ========================================================
     */

    std::cout
        << "\n"
        << "============================================================\n"
        << "MULTI-DEVICE HARDWARE E2E RESULTS\n"
        << "============================================================\n";


    for (const auto& device : devices) {
        std::cout
            << "\nDevice "
            << device.index
            << '\n'
            << "  port:             "
            << device.port
            << '\n'
            << "  identity:         "
            << (
                device.identity.empty()
                    ? "<unknown>"
                    : device.identity
            )
            << '\n'
            << "  device ID:        "
            << (
                device.device_id.empty()
                    ? "<unknown>"
                    : device.device_id
            )
            << '\n'
            << "  initial reset:    "
            << (
                device.initial_reset_completed
                    ? "yes"
                    : "NO"
            )
            << '\n'
            << "  recording start:  "
            << (
                device.recording_started
                    ? "yes"
                    : "NO"
            )
            << '\n'
            << "  recording stop:   "
            << (
                device.recording_stopped
                    ? "yes"
                    : "NO"
            )
            << '\n'
            << "  sync:             "
            << (
                device.sync_completed
                    ? "yes"
                    : "NO"
            )
            << '\n'
            << "  IMU samples:      "
            << device.imu_samples
            << '\n'
            << "  Xsens rows:       "
            << device.xsens_rows
            << '\n'
            << "  legacy rows:      "
            << device.legacy_rows
            << '\n'
            << "  battery rows:     "
            << device.battery_rows
            << '\n'
            << "  final reset:      "
            << (
                device.final_reset_completed
                    ? "yes"
                    : "NO"
            )
            << '\n'
            << "  errors:           "
            << device.errors.size()
            << '\n';


        for (const auto& error : device.errors) {
            std::cout
                << "    - "
                << error
                << '\n';
        }
    }


    const std::size_t failures =
        totalFailures(
            devices
        );


    if (failures != 0) {
        std::cerr
            << "\n"
            << failures
            << " hardware integration assertion(s) FAILED\n"
            << "\nCSV artifacts preserved at:\n  "
            << output_dir
            << '\n';

        return 1;
    }


    /*
     * Remove temporary files only when every device passed.
     */
    std::error_code error;

    std::filesystem::remove_all(
        output_dir,
        error
    );


    std::cout
        << "\n"
        << "PASS: every MMS+ completed the full lifecycle\n"
        << "PASS: every MMS+ recorded real 50 Hz data\n"
        << "PASS: every MMS+ downloaded valid CSV data\n"
        << "PASS: all device IDs were unique\n"
        << "PASS: no device output paths collided\n"
        << "PASS: every MMS+ survived final reset\n\n"
        << "MULTI-DEVICE HARDWARE END-TO-END TEST PASSED\n";


    return 0;
}