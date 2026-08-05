#include "headmotion/app/Commands.hpp"
#include "headmotion/app/DevicePortResolver.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct RecordStartArguments {
    std::optional<std::string> port;
    float sample_rate_hz = 50.0f;
    std::uint32_t battery_interval_seconds = 0;
};

struct SyncArguments {
    std::optional<std::string> port;
    std::string output_path = "data/sync";
};

struct PortAndPayloadArguments {
    std::optional<std::string> port;
    std::string payload;
};

void printUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " scan\n"
        << "  " << argv0 << " identify [--port <serial-port>]\n"
        << "  " << argv0
        << " tx-raw [--port <serial-port>] <hex-bytes>\n"
        << "  " << argv0
        << " cmd [--port <serial-port>] <payload-hex>\n"
        << "  " << argv0 << " module-info [--port <serial-port>]\n"
        << "  " << argv0 << " sdk-probe [--port <serial-port>]\n"
        << "  " << argv0
        << " record-start [--port <serial-port>]"
        << " [--rate <hz>]"
        << " [--battery-interval <seconds>]\n"
        << "  " << argv0 << " record-stop [--port <serial-port>]\n"
        << "  " << argv0
        << " sync [--port <serial-port>] [--out <directory>]\n"
        << "  " << argv0 << " record-reset [--port <serial-port>]\n"
        << "\n"
        << "Run `"
        << argv0
        << " scan` to discover and save the default MMS+ port.\n"
        << "Omit --port to use the saved default port.\n"
        << "\n"
        << "Examples:\n"
        << "  " << argv0 << " scan\n"
        << "  " << argv0 << " identify\n"
        << "  " << argv0 << " record-reset\n"
        << "  " << argv0 << " record-start --rate 50\n"
        << "  " << argv0 << " record-stop\n"
        << "  " << argv0 << " sync --out data/session_001\n"
        << "  " << argv0
        << " identify --port /dev/ttyACM0\n";
}

void setPort(
    std::optional<std::string>& port,
    const char* value
) {
    if (port.has_value()) {
        throw std::runtime_error(
            "--port was specified more than once"
        );
    }

    if (value == nullptr || std::string(value).empty()) {
        throw std::runtime_error(
            "--port requires a non-empty serial-port value"
        );
    }

    port = value;
}

bool consumePortOption(
    int argc,
    char** argv,
    int& index,
    std::optional<std::string>& port
) {
    const std::string option = argv[index];

    if (option != "--port") {
        return false;
    }

    if (index + 1 >= argc) {
        throw std::runtime_error(
            "--port requires a serial-port value"
        );
    }

    setPort(port, argv[index + 1]);
    index += 2;

    return true;
}

std::optional<std::string> parsePortOnlyArguments(
    int argc,
    char** argv,
    const std::string& command
) {
    std::optional<std::string> port;

    int index = 2;

    while (index < argc) {
        if (consumePortOption(
                argc,
                argv,
                index,
                port
            )) {
            continue;
        }

        throw std::runtime_error(
            "Unknown option for " +
            command +
            ": " +
            argv[index]
        );
    }

    return port;
}

PortAndPayloadArguments parsePortAndPayloadArguments(
    int argc,
    char** argv,
    const std::string& command,
    const std::string& payload_name
) {
    PortAndPayloadArguments arguments;

    int index = 2;

    while (index < argc) {
        if (consumePortOption(
                argc,
                argv,
                index,
                arguments.port
            )) {
            continue;
        }

        const std::string argument = argv[index];

        if (!argument.empty() && argument.front() == '-') {
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

        arguments.payload = argument;
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
        value = std::stof(text, &consumed);
    } catch (const std::exception&) {
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
        value = std::stoul(text, &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error(
            "Invalid value for " +
            option_name +
            ": " +
            text
        );
    }

    if (consumed != text.size() || value == 0) {
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

    return static_cast<std::uint32_t>(value);
}

RecordStartArguments parseRecordStartArguments(
    int argc,
    char** argv
) {
    RecordStartArguments arguments;

    int index = 2;

    while (index < argc) {
        if (consumePortOption(
                argc,
                argv,
                index,
                arguments.port
            )) {
            continue;
        }

        const std::string option = argv[index];

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

        if (option == "--battery-interval") {
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
        if (consumePortOption(
                argc,
                argv,
                index,
                arguments.port
            )) {
            continue;
        }

        const std::string option = argv[index];

        if (option == "--out") {
            if (index + 1 >= argc) {
                throw std::runtime_error(
                    "--out requires a directory"
                );
            }

            arguments.output_path = argv[index + 1];

            if (arguments.output_path.empty()) {
                throw std::runtime_error(
                    "--out requires a non-empty directory"
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
    const std::optional<std::string>& explicit_port
) {
    return headmotion::app::resolveDevicePort(
        explicit_port
    );
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            printUsage(argv[0]);
            return 1;
        }

        const std::string command = argv[1];

        if (command == "scan") {
            if (argc != 2) {
                throw std::runtime_error(
                    "scan does not accept any arguments"
                );
            }

            return headmotion::app::runScanPortsCommand();
        }

        if (command == "identify") {
            const auto arguments =
                parsePortOnlyArguments(
                    argc,
                    argv,
                    command
                );

            return headmotion::app::runIdentifyCommand(
                resolvePort(arguments)
            );
        }

        if (command == "module-info") {
            const auto arguments =
                parsePortOnlyArguments(
                    argc,
                    argv,
                    command
                );

            return headmotion::app::runModuleInfoCommand(
                resolvePort(arguments)
            );
        }

        if (command == "sdk-probe") {
            const auto arguments =
                parsePortOnlyArguments(
                    argc,
                    argv,
                    command
                );

            return headmotion::app::runSdkProbeCommand(
                resolvePort(arguments)
            );
        }

        if (command == "record-stop") {
            const auto arguments =
                parsePortOnlyArguments(
                    argc,
                    argv,
                    command
                );

            return headmotion::app::runRecordStopCommand(
                resolvePort(arguments)
            );
        }

        if (command == "record-reset") {
            const auto arguments =
                parsePortOnlyArguments(
                    argc,
                    argv,
                    command
                );

            return headmotion::app::runRecordResetCommand(
                resolvePort(arguments)
            );
        }

        if (command == "tx-raw") {
            const auto arguments =
                parsePortAndPayloadArguments(
                    argc,
                    argv,
                    command,
                    "hex byte string"
                );

            return headmotion::app::runRawTxCommand(
                resolvePort(arguments.port),
                arguments.payload
            );
        }

        if (command == "cmd") {
            const auto arguments =
                parsePortAndPayloadArguments(
                    argc,
                    argv,
                    command,
                    "payload"
                );

            return headmotion::app::runCommandPayloadCommand(
                resolvePort(arguments.port),
                arguments.payload
            );
        }

        if (command == "record-start") {
            const RecordStartArguments arguments =
                parseRecordStartArguments(
                    argc,
                    argv
                );

            return headmotion::app::runRecordStartCommand(
                resolvePort(arguments.port),
                arguments.sample_rate_hz,
                arguments.battery_interval_seconds
            );
        }

        if (command == "sync") {
            const SyncArguments arguments =
                parseSyncArguments(
                    argc,
                    argv
                );

            return headmotion::app::runSyncCommand(
                resolvePort(arguments.port),
                arguments.output_path
            );
        }

        std::cerr
            << "Unknown command: "
            << command
            << "\n\n";

        printUsage(argv[0]);
        return 1;
    } catch (const std::exception& error) {
        std::cerr
            << "ERROR: "
            << error.what()
            << "\n";

        return 1;
    }
}