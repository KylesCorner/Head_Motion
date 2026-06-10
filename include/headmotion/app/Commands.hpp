#pragma once

#include <string>

namespace headmotion::app {

int runScanPortsCommand();
int runIdentifyCommand(const std::string& port_name);
int runRawTxCommand(const std::string& port_name, const std::string& hex_string);
int runCommandPayloadCommand(const std::string& port_name, const std::string& payload_hex);
int runModuleInfoCommand(const std::string& port_name);
int runSdkProbeCommand(const std::string& port_name);
int runRecordStartCommand(const std::string& port_name, float sample_rate_hz);
int runRecordStopCommand(const std::string& port_name);
int runSyncCommand(const std::string& port_name, const std::string& output_path);
int runRecordResetCommand(const std::string& port_name);
} // namespace headmotion::app