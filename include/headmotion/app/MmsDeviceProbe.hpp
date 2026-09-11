#pragma once

#include <optional>
#include <string>

namespace headmotion::app {

    struct MmsProbeResult {
        std::string identity;
    };

    /*
     * Extract the stable board serial from the MMS identity response.
     *
     * Example:
     *   "MbientLab MetaMotionS 8 0.1 1.7.2 0561F6"
     * becomes:
     *   "0561F6"
     *
     * Returns an empty string if no usable final token is present.
     */
    std::string deviceIdFromMmsIdentity(
        const std::string& identity
    );

    std::optional<MmsProbeResult> probeMmsDevice(
        const std::string& port_name
    );

} // namespace headmotion::app
