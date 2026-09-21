#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/app/Commands.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int SKIP_TEST = 77;

std::mutex console_mutex;

struct DeviceResult {
    std::string port;
    std::filesystem::path output_dir;

    std::vector<std::string> failures;

    std::atomic<std::uint64_t> events_received{0};
    std::atomic<bool> output_crosstalk{false};

    std::uint64_t operations_passed = 0;
};

void logLine(
    const std::string& port,
    const std::string& message
) {
    std::lock_guard<std::mutex> lock(console_mutex);

    std::cout
        << "["
        << port
        << "] "
        << message
        << '\n';
}

std::string trim(std::string value)
{
    const auto is_not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    value.erase(
        value.begin(),
        std::find_if(
            value.begin(),
            value.end(),
            is_not_space
        )
    );

    value.erase(
        std::find_if(
            value.rbegin(),
            value.rend(),
            is_not_space
        ).base(),
        value.end()
    );

    return value;
}

std::vector<std::string> parsePorts()
{
    const char* env =
        std::getenv("HEADMOTION_TEST_PORTS");

    if (env == nullptr || *env == '\0') {
        return {};
    }

    std::vector<std::string> ports;

    std::string current;

    for (const char ch : std::string(env)) {
        if (
            ch == ',' ||
            ch == ';' ||
            ch == '\n'
        ) {
            current = trim(current);

            if (!current.empty()) {
                ports.push_back(current);
            }

            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    current = trim(current);

    if (!current.empty()) {
        ports.push_back(current);
    }

    return ports;
}

unsigned int recordingSeconds()
{
    const char* env =
        std::getenv(
            "HEADMOTION_PARALLEL_RECORD_SECONDS"
        );

    if (env == nullptr || *env == '\0') {
        return 3;
    }

    try {
        const auto seconds =
            std::stoul(env);

        return static_cast<unsigned int>(
            std::max<unsigned long>(
                seconds,
                1
            )
        );
    }
    catch (...) {
        return 3;
    }
}

std::string sanitize(
    std::string value
) {
    for (char& ch : value) {
        const auto uch =
            static_cast<unsigned char>(ch);

        if (!std::isalnum(uch)) {
            ch = '_';
        }
    }

    return value;
}

std::filesystem::path makeOutputRoot()
{
    if (
        const char* env =
            std::getenv("HEADMOTION_TEST_OUT")
    ) {
        if (*env != '\0') {
            return env;
        }
    }

    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    return
        std::filesystem::path(
            "test-artifacts"
        ) /
        (
            "parallel_operations_" +
            std::to_string(timestamp)
        );
}

void recordFailure(
    DeviceResult& result,
    const std::string& operation,
    const std::string& message
) {
    result.failures.push_back(
        operation +
        ": " +
        message
    );

    logLine(
        result.port,
        "FAIL " +
        operation +
        ": " +
        message
    );
}

template<typename Function>
bool runOperation(
    DeviceResult& result,
    const std::string& label,
    const std::string& command_name,
    Function&& function
) {
    logLine(
        result.port,
        "START " + label
    );

    const auto log_path =
        result.output_dir /
        (
            label +
            ".jsonl"
        );

    try {
        auto file_sink =
            headmotion::app::CommandOutput::
                jsonFileSink(log_path);

        auto capture_sink =
            [
                &result,
                command_name
            ](
                const headmotion::app::CommandEvent& event
            ) {
                ++result.events_received;

                /*
                 * This is one of the important parallelism checks.
                 *
                 * Events from another operation must never appear
                 * in this CommandOutput.
                 */
                if (
                    event.command !=
                    command_name
                ) {
                    result.output_crosstalk = true;
                }
            };

        headmotion::app::CommandOutput output(
            command_name,
            headmotion::app::CommandOutput::
                combineSinks(
                    {
                        std::move(file_sink),
                        std::move(capture_sink)
                    }
                )
        );

        const int result_code =
            function(output);

        if (result_code != 0) {
            recordFailure(
                result,
                label,
                "exit code " +
                std::to_string(result_code)
            );

            return false;
        }
    }
    catch (const std::exception& error) {
        recordFailure(
            result,
            label,
            error.what()
        );

        return false;
    }
    catch (...) {
        recordFailure(
            result,
            label,
            "unknown exception"
        );

        return false;
    }

    ++result.operations_passed;

    logLine(
        result.port,
        "PASS " + label
    );

    return true;
}

bool containsCsv(
    const std::filesystem::path& directory
) {
    if (!std::filesystem::exists(directory)) {
        return false;
    }

    for (
        const auto& entry :
        std::filesystem::directory_iterator(
            directory
        )
    ) {
        if (
            entry.is_regular_file() &&
            entry.path().extension() == ".csv"
        ) {
            return true;
        }
    }

    return false;
}

void deviceWorker(
    DeviceResult& result,
    std::barrier<>& phase_barrier,
    unsigned int record_seconds
) {
    const auto gatedOperation =
        [
            &result,
            &phase_barrier
        ](
            const std::string& label,
            const std::string& command,
            auto&& operation,
            bool force = false
        ) {
            /*
             * All devices reach this point before any device begins
             * this operation.  This makes the commands genuinely
             * concurrent rather than merely running on several threads.
             */
            phase_barrier.arrive_and_wait();

            if (
                !force &&
                !result.failures.empty()
            ) {
                logLine(
                    result.port,
                    "SKIP " +
                    label +
                    " because an earlier operation failed"
                );

                return false;
            }

            return runOperation(
                result,
                label,
                command,
                std::forward<decltype(operation)>(
                    operation
                )
            );
        };

    /*
     * ---------------------------------------------------------
     * Initial reset
     * ---------------------------------------------------------
     *
     * Run this even if the device has unknown previous state.
     */
    gatedOperation(
        "01_reset_initial",
        "record-reset",
        [&result](
            headmotion::app::CommandOutput& output
        ) {
            return
                headmotion::app::
                    runRecordResetCommand(
                        result.port,
                        output
                    );
        },
        true
    );

    /*
     * Wait for USB re-enumeration.
     *
     * Using /dev/serial/by-id/... on Linux is strongly recommended
     * because /dev/ttyACM0 may become /dev/ttyACM1 after reset.
     */
    phase_barrier.arrive_and_wait();

    std::this_thread::sleep_for(
        2s
    );

    /*
     * ---------------------------------------------------------
     * Identify
     * ---------------------------------------------------------
     */
    gatedOperation(
        "02_identify",
        "identify",
        [&result](
            headmotion::app::CommandOutput& output
        ) {
            return
                headmotion::app::
                    runIdentifyCommand(
                        result.port,
                        output
                    );
        }
    );

    /*
     * ---------------------------------------------------------
     * Module info
     * ---------------------------------------------------------
     */
    gatedOperation(
        "03_module_info",
        "module-info",
        [&result](
            headmotion::app::CommandOutput& output
        ) {
            return
                headmotion::app::
                    runModuleInfoCommand(
                        result.port,
                        output
                    );
        }
    );

    /*
     * ---------------------------------------------------------
     * SDK probe
     * ---------------------------------------------------------
     */
    gatedOperation(
        "04_sdk_probe",
        "sdk-probe",
        [&result](
            headmotion::app::CommandOutput& output
        ) {
            return
                headmotion::app::
                    runSdkProbeCommand(
                        result.port,
                        output
                    );
        }
    );

    /*
     * ---------------------------------------------------------
     * Record start
     * ---------------------------------------------------------
     */
    const bool recording_started =
        gatedOperation(
            "05_record_start",
            "record-start",
            [&result](
                headmotion::app::CommandOutput& output
            ) {
                return
                    headmotion::app::
                        runRecordStartCommand(
                            result.port,

                            /*
                             * 50 Hz is plenty for a short
                             * integration test.
                             */
                            50.0f,

                            /*
                             * Battery logging disabled for
                             * this concurrency test.
                             */
                            0,

                            output
                        );
            }
        );

    /*
     * Make sure all record-start operations have completed
     * before the recording interval begins.
     */
    phase_barrier.arrive_and_wait();

    if (recording_started) {
        logLine(
            result.port,
            "recording for " +
            std::to_string(record_seconds) +
            " seconds"
        );

        std::this_thread::sleep_for(
            std::chrono::seconds(
                record_seconds
            )
        );
    }

    /*
     * ---------------------------------------------------------
     * Record stop
     * ---------------------------------------------------------
     *
     * Even if a later test state has changed, stop the device if
     * we successfully started it.
     */
    phase_barrier.arrive_and_wait();

    if (recording_started) {
        runOperation(
            result,
            "06_record_stop",
            "record-stop",
            [&result](
                headmotion::app::CommandOutput& output
            ) {
                return
                    headmotion::app::
                        runRecordStopCommand(
                            result.port,
                            output
                        );
            }
        );
    }

    /*
     * Ensure every recording has stopped before any sync starts.
     */
    phase_barrier.arrive_and_wait();

    /*
     * ---------------------------------------------------------
     * Sync
     * ---------------------------------------------------------
     */
    bool sync_success = false;

    if (result.failures.empty()) {
        const auto sync_dir =
            result.output_dir /
            "sync";

        sync_success =
            runOperation(
                result,
                "07_sync",
                "sync",
                [
                    &result,
                    &sync_dir
                ](
                    headmotion::app::CommandOutput& output
                ) {
                    return
                        headmotion::app::
                            runSyncCommand(
                                result.port,
                                sync_dir.string(),

                                /*
                                 * Exercise both Xsens and legacy
                                 * IMU output.
                                 */
                                true,

                                output
                            );
                }
            );

        if (
            sync_success &&
            !containsCsv(sync_dir)
        ) {
            recordFailure(
                result,
                "07_sync",
                "sync succeeded but produced no CSV files"
            );

            sync_success = false;
        }
    }

    /*
     * No device begins cleanup until every device has finished
     * the sync phase.
     */
    phase_barrier.arrive_and_wait();

    /*
     * ---------------------------------------------------------
     * Final cleanup reset
     * ---------------------------------------------------------
     *
     * Always attempt this, even if an earlier operation failed.
     */
    runOperation(
        result,
        "08_reset_cleanup",
        "record-reset",
        [&result](
            headmotion::app::CommandOutput& output
        ) {
            return
                headmotion::app::
                    runRecordResetCommand(
                        result.port,
                        output
                    );
        }
    );
}

} // namespace

int main()
{
    const std::vector<std::string> ports =
        parsePorts();

    if (ports.size() < 2) {
        std::cout
            << "SKIP: parallel hardware test requires at least "
            << "two MMS+ devices.\n\n"
            << "Set HEADMOTION_TEST_PORTS, for example:\n\n"
            << "  HEADMOTION_TEST_PORTS="
            << "\"/dev/serial/by-id/device1,"
            << "/dev/serial/by-id/device2\"\n";

        return SKIP_TEST;
    }

    const unsigned int record_seconds =
        recordingSeconds();

    const std::filesystem::path output_root =
        makeOutputRoot();

    std::filesystem::create_directories(
        output_root
    );

    std::cout
        << "HeadMotion parallel operations test\n"
        << "Devices:        "
        << ports.size()
        << '\n'
        << "Record time:    "
        << record_seconds
        << " seconds\n"
        << "Artifacts:      "
        << output_root
        << "\n\n";

    std::vector<std::unique_ptr<DeviceResult>>
        results;

    results.reserve(
        ports.size()
    );

    for (
        std::size_t i = 0;
        i < ports.size();
        ++i
    ) {
        auto result =
            std::make_unique<DeviceResult>();

        result->port =
            ports[i];

        result->output_dir =
            output_root /
            (
                std::to_string(i) +
                "_" +
                sanitize(ports[i])
            );

        std::filesystem::create_directories(
            result->output_dir
        );

        results.push_back(
            std::move(result)
        );
    }

    /*
     * One participant per MMS+ device.
     */
    std::barrier phase_barrier(
        static_cast<std::ptrdiff_t>(
            ports.size()
        )
    );

    std::vector<std::thread> workers;

    workers.reserve(
        ports.size()
    );

    for (auto& result : results) {
        workers.emplace_back(
            [
                &result,
                &phase_barrier,
                record_seconds
            ] {
                deviceWorker(
                    *result,
                    phase_barrier,
                    record_seconds
                );
            }
        );
    }

    for (auto& worker : workers) {
        worker.join();
    }

    std::cout
        << "\n"
        << "========================================\n"
        << "Parallel operations summary\n"
        << "========================================\n";

    bool all_passed = true;

    for (const auto& result : results) {
        std::cout
            << "\nDevice: "
            << result->port
            << '\n'
            << "  operations passed: "
            << result->operations_passed
            << '\n'
            << "  events received:   "
            << result->events_received.load()
            << '\n';

        if (result->output_crosstalk.load()) {
            std::cout
                << "  FAIL: CommandOutput crosstalk detected\n";

            all_passed = false;
        }
        else {
            std::cout
                << "  PASS: no CommandOutput crosstalk\n";
        }

        if (!result->failures.empty()) {
            all_passed = false;

            std::cout
                << "  failures:\n";

            for (
                const auto& failure :
                result->failures
            ) {
                std::cout
                    << "    - "
                    << failure
                    << '\n';
            }
        }
        else {
            std::cout
                << "  PASS: no command failures\n";
        }
    }

    std::cout
        << "\nArtifacts preserved at:\n  "
        << output_root
        << '\n';

    if (!all_passed) {
        std::cout
            << "\nPARALLEL OPERATIONS TEST FAILED\n";

        return 1;
    }

    std::cout
        << "\nPARALLEL OPERATIONS TEST PASSED\n";

    return 0;
}