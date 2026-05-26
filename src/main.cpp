#include "headmotion/app/ConsoleLogger.h"
#include "headmotion/app/InfoService.h"
#include "headmotion/app/RecordStartService.h"
#include "headmotion/app/ScanService.h"

#include "headmotion/ble/MetaWearBoardAdapter.h"
#include "headmotion/ble/SimpleBleAdapter.h"
#include "headmotion/ble/SimpleBleInfoBoard.h"

#include "headmotion/core/BleDeviceInfo.h"
#include "headmotion/core/Result.h"
#include "headmotion/core/SessionConfig.h"
#include "headmotion/core/SessionMetadata.h"

#include "headmotion/storage/FileMetadataStore.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace headmotion;

class CsvSessionWriter {
public:
    Result open(const std::filesystem::path& root, const SessionMetadata& metadata) {
        try {
            const std::filesystem::path deviceDir =
                root / (metadata.serialNumber.empty() ? metadata.deviceMac : metadata.serialNumber);

            std::filesystem::create_directories(deviceDir);

            accelPartial_ = deviceDir / (metadata.sessionId + "_accel.csv.partial");
            gyroPartial_ = deviceDir / (metadata.sessionId + "_gyro.csv.partial");

            accelFinal_ = deviceDir / (metadata.sessionId + "_accel.csv");
            gyroFinal_ = deviceDir / (metadata.sessionId + "_gyro.csv");

            accel_.open(accelPartial_);
            gyro_.open(gyroPartial_);

            if (!accel_) {
                return Result::error("Failed to open accel CSV: " + accelPartial_.string());
            }

            if (!gyro_) {
                return Result::error("Failed to open gyro CSV: " + gyroPartial_.string());
            }

            accel_ << "epoch_ms,x_g,y_g,z_g\n";
            gyro_ << "epoch_ms,x_dps,y_dps,z_dps\n";

            return Result::success();
        } catch (const std::exception& e) {
            return Result::error(std::string("Failed to open CSV files: ") + e.what());
        }
    }

    Result write(const ImuSample& sample) {
        std::ofstream* out = nullptr;

        if (sample.kind == ImuSample::Kind::Accel) {
            out = &accel_;
            accelCount_++;
        } else if (sample.kind == ImuSample::Kind::Gyro) {
            out = &gyro_;
            gyroCount_++;
        } else {
            return Result::success();
        }

        if (out == nullptr || !(*out)) {
            return Result::error("CSV stream is not open");
        }

        (*out) << sample.epochMs << ","
               << sample.x << ","
               << sample.y << ","
               << sample.z << "\n";

        return Result::success();
    }

    Result closeAndCommit() {
        accel_.flush();
        gyro_.flush();

        accel_.close();
        gyro_.close();

        if (accelCount_ == 0 && gyroCount_ == 0) {
            return Result::error("Download completed but no samples were written");
        }

        try {
            if (std::filesystem::exists(accelFinal_)) {
                std::filesystem::remove(accelFinal_);
            }

            if (std::filesystem::exists(gyroFinal_)) {
                std::filesystem::remove(gyroFinal_);
            }

            std::filesystem::rename(accelPartial_, accelFinal_);
            std::filesystem::rename(gyroPartial_, gyroFinal_);

            return Result::success();
        } catch (const std::exception& e) {
            return Result::error(std::string("Failed to commit CSV files: ") + e.what());
        }
    }

    size_t accelCount() const {
        return accelCount_;
    }

    size_t gyroCount() const {
        return gyroCount_;
    }

private:
    std::ofstream accel_;
    std::ofstream gyro_;

    std::filesystem::path accelPartial_;
    std::filesystem::path gyroPartial_;
    std::filesystem::path accelFinal_;
    std::filesystem::path gyroFinal_;

    size_t accelCount_ = 0;
    size_t gyroCount_ = 0;
};

static void printUsage() {
    std::cout << "HeadMotion CLI\n\n";
    std::cout << "Usage:\n";
    std::cout << "  headmotion scan\n";
    std::cout << "  headmotion info <target>\n";
    std::cout << "  headmotion record-start <target> [--erase] [--acc-hz N] [--gyro-hz N] [--out DIR]\n";
    std::cout << "  headmotion sync <target> [--out DIR] [--no-clear]\n\n";
}

static std::filesystem::path defaultDataDir() {
    const char* home = std::getenv("HOME");

    if (home == nullptr || std::string(home).empty()) {
        return std::filesystem::path("HeadMotion") / "data";
    }

    return std::filesystem::path(home) / "HeadMotion" / "data";
}

static bool parseFloatOption(int argc,
                             char** argv,
                             int& index,
                             float& valueOut,
                             const std::string& optionName) {
    if (index + 1 >= argc) {
        std::cerr << "Missing value for " << optionName << "\n";
        return false;
    }

    try {
        valueOut = std::stof(argv[index + 1]);
        index += 1;
        return true;
    } catch (const std::exception&) {
        std::cerr << "Invalid numeric value for " << optionName
                  << ": " << argv[index + 1] << "\n";
        return false;
    }
}

static int runScan() {
    ConsoleLogger logger;
    SimpleBleAdapter ble;
    ScanService service(ble, logger);

    std::vector<BleDeviceInfo> devices;

    Result result = service.run(devices);
    if (!result.ok()) {
        logger.error(result.message());
        return 1;
    }

    std::cout << "Found " << devices.size() << " BLE device(s):\n\n";

    for (const auto& device : devices) {
        std::cout << "identifier: " << device.identifier << "\n";
        std::cout << "address:    " << device.address << "\n";
        std::cout << "rssi:       " << device.rssi << "\n";
        std::cout << "\n";
    }

    return 0;
}

static int runInfo(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Missing target for info command.\n";
        return 1;
    }

    const std::string target = argv[2];

    ConsoleLogger logger;
    SimpleBleInfoBoard board;
    InfoService service(board, logger);

    SessionMetadata metadata;

    Result result = service.run(target, metadata);
    if (!result.ok()) {
        logger.error(result.message());
        return 1;
    }

    std::cout << "HeadMotion device info\n\n";
    std::cout << "target:            " << target << "\n";
    std::cout << "device MAC:        " << metadata.deviceMac << "\n";
    std::cout << "manufacturer:      " << metadata.manufacturer << "\n";
    std::cout << "model number:      " << metadata.modelNumber << "\n";
    std::cout << "serial number:     " << metadata.serialNumber << "\n";
    std::cout << "firmware revision: " << metadata.firmwareRevision << "\n";
    std::cout << "hardware revision: " << metadata.hardwareRevision << "\n";

    return 0;
}

static int runRecordStart(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Missing target for record-start command.\n";
        return 1;
    }

    const std::string target = argv[2];

    SessionConfig config;
    std::filesystem::path outDir = defaultDataDir();

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--erase") {
            config.eraseBeforeStart = true;
        } else if (arg == "--acc-hz") {
            if (!parseFloatOption(argc, argv, i, config.accelHz, arg)) {
                return 1;
            }
        } else if (arg == "--gyro-hz") {
            if (!parseFloatOption(argc, argv, i, config.gyroHz, arg)) {
                return 1;
            }
        } else if (arg == "--out") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --out\n";
                return 1;
            }

            outDir = std::filesystem::path(argv[i + 1]);
            i += 1;
        } else {
            std::cerr << "Unknown record-start option: " << arg << "\n";
            return 1;
        }
    }

    ConsoleLogger logger;
    MetaWearBoardAdapter board;
    FileMetadataStore metadataStore(outDir);
    RecordStartService service(board, metadataStore, logger);

    logger.info("Starting HeadMotion record-start");
    logger.info("Target: " + target);
    logger.info("Output directory: " + outDir.string());

    Result result = service.run(target, config);
    if (!result.ok()) {
        logger.error(result.message());
        return 1;
    }

    std::cout << "\nrecord-start complete\n";
    std::cout << "target:        " << target << "\n";
    std::cout << "accel Hz:      " << config.accelHz << "\n";
    std::cout << "gyro Hz:       " << config.gyroHz << "\n";
    std::cout << "erase:         " << (config.eraseBeforeStart ? "true" : "false") << "\n";
    std::cout << "metadata root: " << outDir << "\n";

    return 0;
}

static int runSync(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Missing target for sync command.\n";
        return 1;
    }

    const std::string target = argv[2];

    std::filesystem::path outDir = defaultDataDir();
    bool clearAfterSuccess = true;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--out") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --out\n";
                return 1;
            }

            outDir = std::filesystem::path(argv[i + 1]);
            i += 1;
        } else if (arg == "--no-clear") {
            clearAfterSuccess = false;
        } else {
            std::cerr << "Unknown sync option: " << arg << "\n";
            return 1;
        }
    }

    ConsoleLogger logger;
    MetaWearBoardAdapter board;
    FileMetadataStore metadataStore(outDir);

    logger.info("Starting HeadMotion sync");
    logger.info("Target: " + target);
    logger.info("Output directory: " + outDir.string());

    auto metadataOpt = metadataStore.loadLatestForDevice(target);
    if (!metadataOpt.has_value()) {
        logger.error("No session metadata found. Run record-start first.");
        return 1;
    }

    SessionMetadata metadata = *metadataOpt;
    logger.info("Loaded session: " + metadata.sessionId);

    auto boardStateOpt = metadataStore.loadBoardState(metadata);
    if (!boardStateOpt.has_value()) {
        logger.error("No saved MetaWear board state found for session. Run record-start again.");
        return 1;
    }

    Result result = board.connect(target);
    if (!result.ok()) {
        logger.error(result.message());
        return 1;
    }

    result = board.deserializeBoardState(*boardStateOpt);
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    result = board.initialize();
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    SessionMetadata deviceInfo;
    result = board.readDeviceInfo(deviceInfo);
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    result = board.stopLogging();
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    result = board.flushLogPage();
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    CsvSessionWriter writer;
    result = writer.open(outDir, metadata);
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    result = board.subscribeLoggers(
        metadata,
        [&writer](const ImuSample& sample) {
            Result writeResult = writer.write(sample);
            if (!writeResult.ok()) {
                std::cerr << "[CSV] " << writeResult.message() << "\n";
            }
        }
    );

    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    result = board.downloadLogs(
        [&logger](const DownloadProgress& progress) {
            logger.info(
                "Download progress: entries_left=" +
                std::to_string(progress.entriesLeft) +
                " total_entries=" +
                std::to_string(progress.totalEntries)
            );
        }
    );

    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    result = writer.closeAndCommit();
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    logger.info("Wrote accel samples: " + std::to_string(writer.accelCount()));
    logger.info("Wrote gyro samples: " + std::to_string(writer.gyroCount()));

    result = metadataStore.markDownloaded(metadata.sessionId);
    if (!result.ok()) {
        logger.error(result.message());
        board.disconnect();
        return 1;
    }

    if (clearAfterSuccess) {
        result = board.clearLogEntries();
        if (!result.ok()) {
            logger.error(result.message());
            board.disconnect();
            return 1;
        }

        logger.info("Cleared device log entries after successful verified download");
    } else {
        logger.warn("Leaving device log entries intact because --no-clear was passed");
    }

    board.disconnect();

    std::cout << "\nsync complete\n";
    std::cout << "session:       " << metadata.sessionId << "\n";
    std::cout << "accel samples: " << writer.accelCount() << "\n";
    std::cout << "gyro samples:  " << writer.gyroCount() << "\n";
    std::cout << "metadata root: " << outDir << "\n";

    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 0;
    }

    const std::string command = argv[1];

    if (command == "--help" || command == "-h" || command == "help") {
        printUsage();
        return 0;
    }

    if (command == "scan") {
        return runScan();
    }

    if (command == "info") {
        return runInfo(argc, argv);
    }

    if (command == "record-start") {
        return runRecordStart(argc, argv);
    }

    if (command == "sync") {
        return runSync(argc, argv);
    }

    std::cerr << "Unknown command: " << command << "\n\n";
    printUsage();
    return 1;
}
