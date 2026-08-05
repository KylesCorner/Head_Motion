#pragma once

#include <optional>
#include <string>

namespace headmotion::app {

struct MmsProbeResult {
    std::string identity;
};

std::optional<MmsProbeResult> probeMmsDevice(
    const std::string& port_name
);

} // namespace headmotion::app