#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace headmotion::app {

int runScanPortsCommand();
int runIdentifyCommand(const std::string& port_name);
int runRawTxCommand(const std::string& port_name, const std::string& hex_string);
int runCommandPayloadCommand(const std::string& port_name, const std::string& payload_hex);
int runModuleInfoCommand(const std::string& port_name);
int runSdkProbeCommand(const std::string& port_name);
using SyncProgressCallback =
    std::function<void(
        std::uint32_t entries_left,
        std::uint32_t total_entries
    )>;

int runRecordStartCommand(
    const std::string& port_name,
    float sample_rate_hz,
    std::uint32_t battery_interval_seconds
);

int runSyncCommand(
    const std::string& port_name,
    const std::string& output_path,
    SyncProgressCallback progress_callback = {}
);

// int runSyncCommand(const std::string& port_name, const std::string& output_path);
int runRecordStopCommand(const std::string& port_name);
int runRecordResetCommand(const std::string& port_name);

} // namespace headmotion::app