#include "headmotion/app/Commands.hpp"
#include "headmotion/app/DevicePortResolver.hpp"
#include "headmotion/app/MmsDeviceProbe.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/transport/SerialPortInfo.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <utility>

namespace {

    enum class CliExitCode : int {
        success = 0,
        usage = 2,
        no_device = 3,
        ambiguous_device = 4,
        device_id_not_found = 5,
        selector_conflict = 6,
        backend_failure = 20,
        unexpected_failure = 21
    };

    struct CommonArguments {
        std::optional<std::string> port;
        std::optional<std::string> device_id;
        bool json = false;
        bool quiet = false;
    };

    struct RecordStartArguments {
        CommonArguments common;
        float sample_rate_hz = 50.0f;
        std::uint32_t battery_interval_seconds = 0;
    };

    struct SyncArguments {
        CommonArguments common;
        std::string output_path = "data/sync";
    };

    struct PortAndPayloadArguments {
        CommonArguments common;
        std::string payload;
    };

    class BackendExecutionError final : public std::runtime_error {
    public:
        explicit BackendExecutionError(
            const std::string& message
        )
            : std::runtime_error(message) {
        }
    };

    class NullBuffer final : public std::streambuf {
    protected:
        int overflow(int ch) override {
            return ch;
        }
    };

    class ScopedCommandOutput {
    public:
        explicit ScopedCommandOutput(bool suppress)
            : suppress_(suppress) {

            if (!suppress_) {
                return;
            }

            old_cout_ = std::cout.rdbuf(&null_buffer_);
            old_cerr_ = std::cerr.rdbuf(&null_buffer_);
        }

        ~ScopedCommandOutput() {
            if (!suppress_) {
                return;
            }

            std::cout.rdbuf(old_cout_);
            std::cerr.rdbuf(old_cerr_);
        }

        ScopedCommandOutput(
            const ScopedCommandOutput&
        ) = delete;

        ScopedCommandOutput& operator=(
            const ScopedCommandOutput&
            ) = delete;

    private:
        bool suppress_ = false;
        NullBuffer null_buffer_;
        std::streambuf* old_cout_ = nullptr;
        std::streambuf* old_cerr_ = nullptr;
    };

    std::string jsonEscape(
        const std::string& value
    ) {
        std::ostringstream out;

        for (const char ch : value) {
            switch (ch) {
            case '\\':
                out << "\\\\";
                break;

            case '"':
                out << "\\\"";
                break;

            case '\b':
                out << "\\b";
                break;

            case '\f':
                out << "\\f";
                break;

            case '\n':
                out << "\\n";
                break;

            case '\r':
                out << "\\r";
                break;

            case '\t':
                out << "\\t";
                break;

            default:
                if (
                    static_cast<unsigned char>(ch) <
                    0x20
                    ) {
                    out << "?";
                }
                else {
                    out << ch;
                }

                break;
            }
        }

        return out.str();
    }

    void emitJson(
        const std::string& json_line,
        std::streambuf* output_buffer
    ) {
        std::ostream out(output_buffer);

        out
            << json_line
            << '\n';

        out.flush();
    }

    void emitStarted(
        const std::string& command,
        const std::optional<std::string>& port,
        const std::optional<std::string>& device_id,
        std::streambuf* output_buffer
    ) {
        std::ostringstream line;

        line
            << "{\"event\":\"started\","
            << "\"command\":\""
            << jsonEscape(command)
            << "\"";

        if (port.has_value()) {
            line
                << ",\"port\":\""
                << jsonEscape(*port)
                << "\"";
        }

        if (device_id.has_value()) {
            line
                << ",\"device_id\":\""
                << jsonEscape(*device_id)
                << "\"";
        }

        line << "}";

        emitJson(
            line.str(),
            output_buffer
        );
    }

    void emitCompleted(
        const std::string& command,
        int backend_exit_code,
        int process_exit_code,
        std::streambuf* output_buffer
    ) {
        std::ostringstream line;

        line
            << "{\"event\":\"completed\","
            << "\"command\":\""
            << jsonEscape(command)
            << "\",\"backend_exit_code\":"
            << backend_exit_code
            << ",\"exit_code\":"
            << process_exit_code
            << "}";

        emitJson(
            line.str(),
            output_buffer
        );
    }

    void emitError(
        const std::string& command,
        const std::string& code,
        const std::string& message,
        int process_exit_code,
        std::streambuf* output_buffer
    ) {
        std::ostringstream line;

        line
            << "{\"event\":\"error\","
            << "\"command\":\""
            << jsonEscape(command)
            << "\",\"code\":\""
            << jsonEscape(code)
            << "\",\"message\":\""
            << jsonEscape(message)
            << "\",\"exit_code\":"
            << process_exit_code
            << "}";

        emitJson(
            line.str(),
            output_buffer
        );
    }

    bool rawFlagPresent(
        int argc,
        char** argv,
        const std::string& flag
    ) {
        for (int index = 2; index < argc; ++index) {
            if (flag == argv[index]) {
                return true;
            }
        }

        return false;
    }

    constexpr std::uint16_t MMS_VENDOR_ID =
        0x1915;

    constexpr std::uint16_t MMS_PRODUCT_ID =
        0xd978;

    bool isMmsCandidate(
        const headmotion::transport::SerialPortInfo& port
    ) {
        return
            (
                port.vendor_id == MMS_VENDOR_ID &&
                port.product_id == MMS_PRODUCT_ID
                ) ||
            port.likely_mms;
    }

    std::string metadataDeviceId(
        const headmotion::transport::SerialPortInfo& port
    ) {
        if (!port.serial_number.empty()) {
            return port.serial_number;
        }

        if (!port.symlink_path.empty()) {
            constexpr const char* marker =
                "usb-MbientLab_MetaMotionS_";

            const std::size_t begin =
                port.symlink_path.find(marker);

            if (begin != std::string::npos) {
                const std::size_t serial_begin =
                    begin +
                    std::string(marker).size();

                const std::size_t serial_end =
                    port.symlink_path.find(
                        "-if",
                        serial_begin
                    );

                if (
                    serial_end != std::string::npos &&
                    serial_end > serial_begin
                    ) {
                    return port.symlink_path.substr(
                        serial_begin,
                        serial_end - serial_begin
                    );
                }
            }
        }

        return {};
    }

    std::string stableDeviceId(
        const headmotion::transport::SerialPortInfo& port,
        const std::string& identity
    ) {
        /*
         * Preferred source: the serial reported by the MMS itself.
         *
         * Example:
         *
         *   MbientLab MetaMotionS 8 0.1 1.7.2 0561F6
         *                                      ^^^^^^
         *
         * Windows does not always expose a useful USB serial through
         * SerialPortInfo, but the MMS identity response does.
         */
        const std::string board_id =
            headmotion::app::deviceIdFromMmsIdentity(
                identity
            );

        if (!board_id.empty()) {
            return board_id;
        }

        return metadataDeviceId(port);
    }

    int runJsonScan(
        std::streambuf* output_buffer
    ) {
        const auto ports =
            headmotion::transport::SerialPortFactory::
            listPorts();

        if (ports.empty()) {
            return 2;
        }

        std::uint32_t verified_count = 0;

        for (const auto& port : ports) {
            if (!isMmsCandidate(port)) {
                continue;
            }

            const auto probe =
                headmotion::app::probeMmsDevice(
                    port.preferredPath()
                );

            if (!probe.has_value()) {
                continue;
            }

            ++verified_count;

            const std::string device_id =
                stableDeviceId(
                    port,
                    probe->identity
                );

            std::ostringstream line;

            line
                << "{\"event\":\"device\","
                << "\"port\":\""
                << jsonEscape(port.preferredPath())
                << "\",\"system_port\":\""
                << jsonEscape(port.path)
                << "\",\"identity\":\""
                << jsonEscape(probe->identity)
                << "\",\"vendor_id\":"
                << port.vendor_id
                << ",\"product_id\":"
                << port.product_id;

            if (!device_id.empty()) {
                line
                    << ",\"device_id\":\""
                    << jsonEscape(device_id)
                    << "\"";
            }

            line << "}";

            emitJson(
                line.str(),
                output_buffer
            );
        }

        std::ostringstream summary;

        summary
            << "{\"event\":\"scan_summary\","
            << "\"verified_devices\":"
            << verified_count
            << "}";

        emitJson(
            summary.str(),
            output_buffer
        );

        return verified_count == 0
            ? 3
            : 0;
    }

    void printUsage(
        const char* argv0
    ) {
        std::cerr
            << "Usage:\n"
            << "  " << argv0
            << " scan [--json] [--quiet]\n"

            << "  " << argv0
            << " identify [device selector] [output options]\n"

            << "  " << argv0
            << " tx-raw [device selector] <hex-bytes> [output options]\n"

            << "  " << argv0
            << " cmd [device selector] <payload-hex> [output options]\n"

            << "  " << argv0
            << " module-info [device selector] [output options]\n"

            << "  " << argv0
            << " sdk-probe [device selector] [output options]\n"

            << "  " << argv0
            << " record-start [device selector]"
            << " [--rate <hz>]"
            << " [--battery-interval <seconds>]"
            << " [output options]\n"

            << "  " << argv0
            << " record-stop [device selector] [output options]\n"

            << "  " << argv0
            << " sync [device selector]"
            << " [--out <directory> | --output-dir <directory>]"
            << " [output options]\n"

            << "  " << argv0
            << " record-reset [device selector] [output options]\n"

            << "\n"
            << "Device selector (choose at most one):\n"
            << "  --port <serial-port>\n"
            << "  --device-id <board-serial>\n"

            << "\n"
            << "Output options:\n"
            << "  --json   emit newline-delimited JSON and suppress command chatter\n"
            << "  --quiet  suppress normal command output\n"

            << "\n"
            << "Exit codes:\n"
            << "  0   success\n"
            << "  2   invalid arguments\n"
            << "  3   no MMS+ available\n"
            << "  4   multiple MMS+ devices; selector required\n"
            << "  5   requested device-id not found\n"
            << "  6   conflicting/invalid device selector\n"
            << "  20  backend command failed\n"
            << "  21  unexpected failure\n";
    }

    void setOptionalValue(
        std::optional<std::string>& destination,
        const char* value,
        const std::string& option_name
    ) {
        if (destination.has_value()) {
            throw std::runtime_error(
                option_name +
                " was specified more than once"
            );
        }

        if (
            value == nullptr ||
            std::string(value).empty()
            ) {
            throw std::runtime_error(
                option_name +
                " requires a non-empty value"
            );
        }

        destination = value;
    }

    bool consumeCommonOption(
        int argc,
        char** argv,
        int& index,
        CommonArguments& common,
        bool allow_device_selector = true
    ) {
        const std::string option =
            argv[index];

        if (option == "--json") {
            common.json = true;
            ++index;
            return true;
        }

        if (option == "--quiet") {
            common.quiet = true;
            ++index;
            return true;
        }

        if (!allow_device_selector) {
            return false;
        }

        if (
            option == "--port" ||
            option == "--device-id"
            ) {
            if (index + 1 >= argc) {
                throw std::runtime_error(
                    option +
                    " requires a value"
                );
            }

            if (option == "--port") {
                setOptionalValue(
                    common.port,
                    argv[index + 1],
                    option
                );
            }
            else {
                setOptionalValue(
                    common.device_id,
                    argv[index + 1],
                    option
                );
            }

            index += 2;
            return true;
        }

        return false;
    }

    CommonArguments parseCommonOnlyArguments(
        int argc,
        char** argv,
        const std::string& command,
        bool allow_device_selector = true
    ) {
        CommonArguments common;

        int index = 2;

        while (index < argc) {
            if (
                consumeCommonOption(
                    argc,
                    argv,
                    index,
                    common,
                    allow_device_selector
                )
                ) {
                continue;
            }

            throw std::runtime_error(
                "Unknown option for " +
                command +
                ": " +
                argv[index]
            );
        }

        return common;
    }

    PortAndPayloadArguments
        parsePortAndPayloadArguments(
            int argc,
            char** argv,
            const std::string& command,
            const std::string& payload_name
        ) {
        PortAndPayloadArguments arguments;

        int index = 2;

        while (index < argc) {
            if (
                consumeCommonOption(
                    argc,
                    argv,
                    index,
                    arguments.common
                )
                ) {
                continue;
            }

            const std::string argument =
                argv[index];

            if (
                !argument.empty() &&
                argument.front() == '-'
                ) {
                throw std::runtime_error(
                    "Unknown option for " +
                    command +
                    ": " +
                    argument
                );
            }

            if (!arguments.payload.empty()) {
                throw std::runtime_error(
                    command +
                    " accepts exactly one " +
                    payload_name
                );
            }

            arguments.payload =
                argument;

            ++index;
        }

        if (arguments.payload.empty()) {
            throw std::runtime_error(
                command +
                " requires " +
                payload_name
            );
        }

        return arguments;
    }

    float parseFloat(
        const std::string& text,
        const std::string& option_name
    ) {
        std::size_t consumed = 0;
        float value = 0.0f;

        try {
            value =
                std::stof(
                    text,
                    &consumed
                );
        }
        catch (const std::exception&) {
            throw std::runtime_error(
                "Invalid value for " +
                option_name +
                ": " +
                text
            );
        }

        if (consumed != text.size()) {
            throw std::runtime_error(
                "Invalid value for " +
                option_name +
                ": " +
                text
            );
        }

        return value;
    }

    std::uint32_t parsePositiveUint32(
        const std::string& text,
        const std::string& option_name
    ) {
        std::size_t consumed = 0;
        unsigned long value = 0;

        try {
            value =
                std::stoul(
                    text,
                    &consumed
                );
        }
        catch (const std::exception&) {
            throw std::runtime_error(
                "Invalid value for " +
                option_name +
                ": " +
                text
            );
        }

        if (
            consumed != text.size() ||
            value == 0
            ) {
            throw std::runtime_error(
                option_name +
                " must be a positive integer"
            );
        }

        if (value > UINT32_MAX) {
            throw std::runtime_error(
                option_name +
                " is too large"
            );
        }

        return static_cast<std::uint32_t>(
            value
            );
    }

    RecordStartArguments parseRecordStartArguments(
        int argc,
        char** argv
    ) {
        RecordStartArguments arguments;

        int index = 2;

        while (index < argc) {
            if (
                consumeCommonOption(
                    argc,
                    argv,
                    index,
                    arguments.common
                )
                ) {
                continue;
            }

            const std::string option =
                argv[index];

            if (option == "--rate") {
                if (index + 1 >= argc) {
                    throw std::runtime_error(
                        "--rate requires a value"
                    );
                }

                arguments.sample_rate_hz =
                    parseFloat(
                        argv[index + 1],
                        "--rate"
                    );

                index += 2;
                continue;
            }

            if (
                option ==
                "--battery-interval"
                ) {
                if (index + 1 >= argc) {
                    throw std::runtime_error(
                        "--battery-interval requires a value"
                    );
                }

                arguments.battery_interval_seconds =
                    parsePositiveUint32(
                        argv[index + 1],
                        "--battery-interval"
                    );

                index += 2;
                continue;
            }

            throw std::runtime_error(
                "Unknown option for record-start: " +
                option
            );
        }

        return arguments;
    }

    SyncArguments parseSyncArguments(
        int argc,
        char** argv
    ) {
        SyncArguments arguments;

        int index = 2;

        while (index < argc) {
            if (
                consumeCommonOption(
                    argc,
                    argv,
                    index,
                    arguments.common
                )
                ) {
                continue;
            }

            const std::string option =
                argv[index];

            if (
                option == "--out" ||
                option == "--output-dir"
                ) {
                if (index + 1 >= argc) {
                    throw std::runtime_error(
                        option +
                        " requires a directory"
                    );
                }

                arguments.output_path =
                    argv[index + 1];

                if (
                    arguments.output_path.empty()
                    ) {
                    throw std::runtime_error(
                        option +
                        " requires a non-empty directory"
                    );
                }

                index += 2;
                continue;
            }

            throw std::runtime_error(
                "Unknown option for sync: " +
                option
            );
        }

        return arguments;
    }

    std::string resolvePort(
        const CommonArguments& common
    ) {
        return
            headmotion::app::resolveDevicePort(
                common.port,
                common.device_id
            );
    }

    template <typename Function>
    int invokeBackend(
        Function&& function
    ) {
        try {
            return
                std::forward<Function>(
                    function
                )();
        }
        catch (const std::exception& error) {
            throw BackendExecutionError(
                error.what()
            );
        }
        catch (...) {
            throw BackendExecutionError(
                "backend command failed with "
                "an unknown exception"
            );
        }
    }

    int normalizeBackendResult(
        const std::string& command,
        int backend_result,
        bool json,
        std::streambuf* json_buffer
    ) {
        const int process_result =
            backend_result == 0
            ? static_cast<int>(
                CliExitCode::success
                )
            : static_cast<int>(
                CliExitCode::backend_failure
                );

        if (json) {
            emitCompleted(
                command,
                backend_result,
                process_result,
                json_buffer
            );
        }

        return process_result;
    }

    int mapDevicePortError(
        const headmotion::app::DevicePortError& error
    ) {
        using Reason =
            headmotion::app::DevicePortErrorReason;

        switch (error.reason()) {
        case Reason::no_serial_ports:
        case Reason::no_mms_devices:
            return static_cast<int>(
                CliExitCode::no_device
                );

        case Reason::multiple_mms_devices:
            return static_cast<int>(
                CliExitCode::ambiguous_device
                );

        case Reason::device_id_not_found:
            return static_cast<int>(
                CliExitCode::device_id_not_found
                );

        case Reason::selector_conflict:
        case Reason::invalid_selector:
            return static_cast<int>(
                CliExitCode::selector_conflict
                );
        }

        return static_cast<int>(
            CliExitCode::unexpected_failure
            );
    }

    std::string devicePortErrorCode(
        const headmotion::app::DevicePortError& error
    ) {
        using Reason =
            headmotion::app::DevicePortErrorReason;

        switch (error.reason()) {
        case Reason::no_serial_ports:
        case Reason::no_mms_devices:
            return "no_device";

        case Reason::multiple_mms_devices:
            return "ambiguous_device";

        case Reason::device_id_not_found:
            return "device_id_not_found";

        case Reason::selector_conflict:
            return "selector_conflict";

        case Reason::invalid_selector:
            return "invalid_selector";
        }

        return "device_error";
    }

} // namespace

int main(
    int argc,
    char** argv
) {
    std::streambuf* const original_cout =
        std::cout.rdbuf();

    const bool requested_json =
        argc >= 2 &&
        rawFlagPresent(
            argc,
            argv,
            "--json"
        );

    const bool requested_quiet =
        argc >= 2 &&
        rawFlagPresent(
            argc,
            argv,
            "--quiet"
        );

    const std::string command =
        argc >= 2
        ? argv[1]
        : "";

    try {
        if (argc < 2) {
            printUsage(argv[0]);

            return static_cast<int>(
                CliExitCode::usage
                );
        }

        if (command == "scan") {
            const CommonArguments common =
                parseCommonOnlyArguments(
                    argc,
                    argv,
                    command,
                    false
                );

            if (common.json) {
                emitStarted(
                    command,
                    std::nullopt,
                    std::nullopt,
                    original_cout
                );
            }

            ScopedCommandOutput output(
                common.json ||
                common.quiet
            );

            const int result =
                common.json
                ? invokeBackend(
                    [&] {
                        return runJsonScan(
                            original_cout
                        );
                    }
                )
                : invokeBackend(
                    [] {
                        return
                            headmotion::app::
                            runScanPortsCommand();
                    }
                );

            return normalizeBackendResult(
                command,
                result,
                common.json,
                original_cout
            );
        }

        if (
            command == "identify" ||
            command == "module-info" ||
            command == "sdk-probe" ||
            command == "record-stop" ||
            command == "record-reset"
            ) {
            const CommonArguments common =
                parseCommonOnlyArguments(
                    argc,
                    argv,
                    command
                );

            const std::string port =
                resolvePort(common);

            if (common.json) {
                emitStarted(
                    command,
                    port,
                    common.device_id,
                    original_cout
                );
            }

            ScopedCommandOutput output(
                common.json ||
                common.quiet
            );

            const int result =
                invokeBackend(
                    [&] {
                        if (
                            command ==
                            "identify"
                            ) {
                            return
                                headmotion::app::
                                runIdentifyCommand(
                                    port
                                );
                        }

                        if (
                            command ==
                            "module-info"
                            ) {
                            return
                                headmotion::app::
                                runModuleInfoCommand(
                                    port
                                );
                        }

                        if (
                            command ==
                            "sdk-probe"
                            ) {
                            return
                                headmotion::app::
                                runSdkProbeCommand(
                                    port
                                );
                        }

                        if (
                            command ==
                            "record-stop"
                            ) {
                            return
                                headmotion::app::
                                runRecordStopCommand(
                                    port
                                );
                        }

                        return
                            headmotion::app::
                            runRecordResetCommand(
                                port
                            );
                    }
                );

            return normalizeBackendResult(
                command,
                result,
                common.json,
                original_cout
            );
        }

        if (
            command == "tx-raw" ||
            command == "cmd"
            ) {
            const PortAndPayloadArguments arguments =
                parsePortAndPayloadArguments(
                    argc,
                    argv,
                    command,
                    command == "tx-raw"
                    ? "hex byte string"
                    : "payload"
                );

            const std::string port =
                resolvePort(
                    arguments.common
                );

            if (arguments.common.json) {
                emitStarted(
                    command,
                    port,
                    arguments.common.device_id,
                    original_cout
                );
            }

            ScopedCommandOutput output(
                arguments.common.json ||
                arguments.common.quiet
            );

            const int result =
                invokeBackend(
                    [&] {
                        if (
                            command ==
                            "tx-raw"
                            ) {
                            return
                                headmotion::app::
                                runRawTxCommand(
                                    port,
                                    arguments.payload
                                );
                        }

                        return
                            headmotion::app::
                            runCommandPayloadCommand(
                                port,
                                arguments.payload
                            );
                    }
                );

            return normalizeBackendResult(
                command,
                result,
                arguments.common.json,
                original_cout
            );
        }

        if (
            command ==
            "record-start"
            ) {
            const RecordStartArguments arguments =
                parseRecordStartArguments(
                    argc,
                    argv
                );

            const std::string port =
                resolvePort(
                    arguments.common
                );

            if (arguments.common.json) {
                emitStarted(
                    command,
                    port,
                    arguments.common.device_id,
                    original_cout
                );
            }

            ScopedCommandOutput output(
                arguments.common.json ||
                arguments.common.quiet
            );

            const int result =
                invokeBackend(
                    [&] {
                        return
                            headmotion::app::
                            runRecordStartCommand(
                                port,
                                arguments.sample_rate_hz,
                                arguments.battery_interval_seconds
                            );
                    }
                );

            return normalizeBackendResult(
                command,
                result,
                arguments.common.json,
                original_cout
            );
        }

        if (command == "sync") {
            const SyncArguments arguments =
                parseSyncArguments(
                    argc,
                    argv
                );

            const std::string port =
                resolvePort(
                    arguments.common
                );

            if (arguments.common.json) {
                emitStarted(
                    command,
                    port,
                    arguments.common.device_id,
                    original_cout
                );
            }

            ScopedCommandOutput output(
                arguments.common.json ||
                arguments.common.quiet
            );

            const int result =
                invokeBackend(
                    [&] {
                        return
                            headmotion::app::
                            runSyncCommand(
                                port,
                                arguments.output_path,
                                [
                                    json =
                                        arguments.common.json,
                                        original_cout
                                ](
                                    std::uint32_t entries_left,
                                    std::uint32_t total_entries
                                    ) {
                                        if (!json) {
                                            return;
                                        }

                                        const double percent =
                                            total_entries == 0
                                            ? 100.0
                                            : 100.0 *
                                            static_cast<double>(
                                                total_entries -
                                                entries_left
                                                ) /
                                            static_cast<double>(
                                                total_entries
                                                );

                                        std::ostringstream line;

                                        line
                                            << "{\"event\":\"progress\","
                                            << "\"command\":\"sync\","
                                            << "\"entries_left\":"
                                            << entries_left
                                            << ",\"total_entries\":"
                                            << total_entries
                                            << ",\"percent\":"
                                            << percent
                                            << "}";

                                        emitJson(
                                            line.str(),
                                            original_cout
                                        );
                                }
                                        );
                    }
                );

            return normalizeBackendResult(
                command,
                result,
                arguments.common.json,
                original_cout
            );
        }

        throw std::runtime_error(
            "Unknown command: " +
            command
        );

    }
    catch (
        const headmotion::app::DevicePortError& error
        ) {
        const int exit_code =
            mapDevicePortError(error);

        if (requested_json) {
            emitError(
                command,
                devicePortErrorCode(error),
                error.what(),
                exit_code,
                original_cout
            );
        }
        else if (!requested_quiet) {
            std::cerr
                << "ERROR: "
                << error.what()
                << "\n";
        }

        return exit_code;

    }
    catch (
        const BackendExecutionError& error
        ) {
        const int exit_code =
            static_cast<int>(
                CliExitCode::backend_failure
                );

        if (requested_json) {
            emitError(
                command,
                "backend_exception",
                error.what(),
                exit_code,
                original_cout
            );
        }
        else if (!requested_quiet) {
            std::cerr
                << "ERROR: "
                << error.what()
                << "\n";
        }

        return exit_code;

    }
    catch (
        const std::runtime_error& error
        ) {
        const int exit_code =
            static_cast<int>(
                CliExitCode::usage
                );

        if (requested_json) {
            emitError(
                command,
                "invalid_arguments",
                error.what(),
                exit_code,
                original_cout
            );
        }
        else if (!requested_quiet) {
            std::cerr
                << "ERROR: "
                << error.what()
                << "\n\n";

            printUsage(argv[0]);
        }

        return exit_code;

    }
    catch (
        const std::exception& error
        ) {
        const int exit_code =
            static_cast<int>(
                CliExitCode::unexpected_failure
                );

        if (requested_json) {
            emitError(
                command,
                "unexpected_failure",
                error.what(),
                exit_code,
                original_cout
            );
        }
        else if (!requested_quiet) {
            std::cerr
                << "ERROR: "
                << error.what()
                << "\n";
        }

        return exit_code;
    }
}
