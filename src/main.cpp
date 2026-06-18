#include "headmotion/app/Commands.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <cstdint>

namespace {

void printUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " scan\n"
        << "  " << argv0 << " identify <serial-port>\n"
        << "  " << argv0 << " tx-raw <serial-port> <hex-bytes>\n"
        << "  " << argv0 << " cmd <serial-port> <payload-hex>\n"
        << "  " << argv0 << " module-info <serial-port>\n"
        << "  " << argv0 << " sdk-probe <serial-port>\n"
        << "  " << argv0 << " record-start <serial-port> [--rate 25|50|100|200|400|800|1600|3200] [--battery-interval seconds]\n"
        << "  " << argv0 << " record-stop <serial-port>\n"
        << "  " << argv0 << " sync <serial-port> [--out path]\n"
        << "  " << argv0 << " record-reset <serial-port>\n"
        << "\n"
        << "Examples:\n"
        << "  " << argv0 << " scan\n"
        << "  " << argv0 << " identify /dev/ttyACM0\n"
        << "  " << argv0 << " tx-raw /dev/ttyACM0 \"1F 02 01 80 0A\"\n"
        << "  " << argv0 << " cmd /dev/ttyACM0 \"01 80\"\n"
        << "  " << argv0 << " module-info /dev/ttyACM0\n"
        << "  " << argv0 << " sdk-probe /dev/ttyACM0\n"
        << "  " << argv0 << " record-start /dev/ttyACM0 --rate 50 --battery-interval 60\n"
        << "  " << argv0 << " record-stop /dev/ttyACM0\n"
        << "  " << argv0 << " sync /dev/ttyACM0 --out data/sync.csv\n"
        << "  " << argv0 << " record-reset /dev/ttyACM0\n";
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
            return headmotion::app::runScanPortsCommand();
        }

        if (command == "identify") {
            if (argc != 3) {
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runIdentifyCommand(argv[2]);
        }

        if (command == "tx-raw") {
            if (argc != 4) {
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runRawTxCommand(argv[2], argv[3]);
        }

        if (command == "cmd") {
            if (argc != 4) {
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runCommandPayloadCommand(argv[2], argv[3]);
        }

        if (command == "module-info") {
            if (argc != 3) {
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runModuleInfoCommand(argv[2]);
        }

        if (command == "sdk-probe") {
            if (argc != 3) {
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runSdkProbeCommand(argv[2]);
        }
        if (command == "record-start") {
            if (argc < 3) {
                printUsage(argv[0]);
                return 1;
            }

            const std::string port_name = argv[2];

            float sample_rate_hz = 50.0f;
            std::uint32_t battery_interval_seconds = 0;

            int i = 3;
            while (i < argc) {
                const std::string option = argv[i];

                if (option == "--rate") {
                    if (i + 1 >= argc) {
                        std::cerr << "--rate requires a value\n";
                        return 1;
                    }

                    try {
                        sample_rate_hz = std::stof(argv[i + 1]);
                    } catch (const std::exception&) {
                        std::cerr << "Invalid sample rate: " << argv[i + 1] << "\n";
                        return 1;
                    }

                    i += 2;
                    continue;
                }

                if (option == "--battery-interval") {
                    if (i + 1 >= argc) {
                        std::cerr << "--battery-interval requires a value in seconds\n";
                        return 1;
                    }

                    try {
                        const int value = std::stoi(argv[i + 1]);

                        if (value <= 0) {
                            std::cerr << "--battery-interval must be greater than zero\n";
                            return 1;
                        }

                        battery_interval_seconds = static_cast<std::uint32_t>(value);
                    } catch (const std::exception&) {
                        std::cerr << "Invalid battery interval: " << argv[i + 1] << "\n";
                        return 1;
                    }

                    i += 2;
                    continue;
                }

                std::cerr << "Unknown option for record-start: " << option << "\n";
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runRecordStartCommand(
                port_name,
                sample_rate_hz,
                battery_interval_seconds
            );
        }

        // if (command == "record-start") {
        //     if (argc != 3 && argc != 5) {
        //         printUsage(argv[0]);
        //         return 1;
        //     }
        //
        //     float sample_rate_hz = 50.0f;
        //
        //     if (argc == 5) {
        //         const std::string option = argv[3];
        //
        //         if (option != "--rate") {
        //             std::cerr << "Unknown option for record-start: " << option << "\n";
        //             printUsage(argv[0]);
        //             return 1;
        //         }
        //
        //         try {
        //             sample_rate_hz = std::stof(argv[4]);
        //         } catch (const std::exception&) {
        //             std::cerr << "Invalid sample rate: " << argv[4] << "\n";
        //             return 1;
        //         }
        //     }
        //
        //     return headmotion::app::runRecordStartCommand(argv[2], sample_rate_hz);
        // }
        if (command == "record-stop") {
            if (argc != 3) {
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runRecordStopCommand(argv[2]);
        }

        if (command == "record-reset") {
            if (argc != 3) {
                printUsage(argv[0]);
                return 1;
            }

            return headmotion::app::runRecordResetCommand(argv[2]);
        }

        if (command == "sync") {
            if (argc != 3 && argc != 5) {
                printUsage(argv[0]);
                return 1;
            }

            std::string output_path = "data/sync.csv";

            if (argc == 5) {
                const std::string option = argv[3];

                if (option != "--out") {
                    std::cerr << "Unknown option for sync: " << option << "\n";
                    printUsage(argv[0]);
                    return 1;
                }

                output_path = argv[4];
            }

            return headmotion::app::runSyncCommand(argv[2], output_path);
        }

        std::cerr << "Unknown command: " << command << "\n";
        printUsage(argv[0]);
        return 1;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
