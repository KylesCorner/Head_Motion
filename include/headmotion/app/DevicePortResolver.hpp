#pragma once

#include <optional>
#include <string>

namespace headmotion::app {

std::string resolveDevicePort(
    const std::optional<std::string>& explicit_port
);

} // namespace headmotion::app