#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace headmotion::app {

    class CommandOutput;

    using SyncProgressCallback =
        std::function<void(
            std::uint32_t entries_left,
            std::uint32_t total_entries
        )>;

    // ---------------------------------------------------------------------
    // Scan
    // ---------------------------------------------------------------------

    int runScanPortsCommand(
        CommandOutput& output
    );

    int runScanPortsCommand();

    // ---------------------------------------------------------------------
    // Identify
    // ---------------------------------------------------------------------

    int runIdentifyCommand(
        const std::string& port_name,
        CommandOutput& output
    );

    int runIdentifyCommand(
        const std::string& port_name
    );

    // ---------------------------------------------------------------------
    // Raw TX
    // ---------------------------------------------------------------------

    int runRawTxCommand(
        const std::string& port_name,
        const std::string& hex_string,
        CommandOutput& output
    );

    int runRawTxCommand(
        const std::string& port_name,
        const std::string& hex_string
    );

    // ---------------------------------------------------------------------
    // Framed command payload
    // ---------------------------------------------------------------------

    int runCommandPayloadCommand(
        const std::string& port_name,
        const std::string& payload_hex,
        CommandOutput& output
    );

    int runCommandPayloadCommand(
        const std::string& port_name,
        const std::string& payload_hex
    );

    // ---------------------------------------------------------------------
    // Module info
    // ---------------------------------------------------------------------

    int runModuleInfoCommand(
        const std::string& port_name,
        CommandOutput& output
    );

    int runModuleInfoCommand(
        const std::string& port_name
    );

    // ---------------------------------------------------------------------
    // SDK probe
    // ---------------------------------------------------------------------

    int runSdkProbeCommand(
        const std::string& port_name,
        CommandOutput& output
    );

    int runSdkProbeCommand(
        const std::string& port_name
    );

    // ---------------------------------------------------------------------
    // Record start
    // ---------------------------------------------------------------------

    int runRecordStartCommand(
        const std::string& port_name,
        float sample_rate_hz,
        std::uint32_t battery_interval_seconds,
        CommandOutput& output
    );

    int runRecordStartCommand(
        const std::string& port_name,
        float sample_rate_hz,
        std::uint32_t battery_interval_seconds
    );

    // ---------------------------------------------------------------------
    // Record stop
    // ---------------------------------------------------------------------

    int runRecordStopCommand(
        const std::string& port_name,
        CommandOutput& output
    );

    int runRecordStopCommand(
        const std::string& port_name
    );

    // ---------------------------------------------------------------------
    // Record reset
    // ---------------------------------------------------------------------

    int runRecordResetCommand(
        const std::string& port_name,
        CommandOutput& output
    );

    int runRecordResetCommand(
        const std::string& port_name
    );

    // ---------------------------------------------------------------------
    // Sync
    // ---------------------------------------------------------------------

    int runSyncCommand(
        const std::string& port_name,
        const std::string& output_path,
        bool write_imu_csv,
        CommandOutput& output,
        SyncProgressCallback progress_callback = {}
    );

    int runSyncCommand(
        const std::string& port_name,
        const std::string& output_path,
        CommandOutput& output,
        SyncProgressCallback progress_callback = {}
    );

    /*
     * Compatibility wrappers. These create the default JSONL stdout sink.
     */
    int runSyncCommand(
        const std::string& port_name,
        const std::string& output_path,
        bool write_imu_csv,
        SyncProgressCallback progress_callback = {}
    );

    int runSyncCommand(
        const std::string& port_name,
        const std::string& output_path,
        SyncProgressCallback progress_callback = {}
    );

} // namespace headmotion::app
