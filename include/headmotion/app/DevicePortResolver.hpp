#pragma once

#include <optional>
#include <stdexcept>
#include <string>

namespace headmotion::app {

enum class DevicePortErrorReason {
    no_serial_ports,
    no_mms_devices,
    multiple_mms_devices,
    device_id_not_found,
    selector_conflict,
    invalid_selector
};

class DevicePortError : public std::runtime_error {
public:
    DevicePortError(
        DevicePortErrorReason reason,
        const std::string& message
    );

    [[nodiscard]]
    DevicePortErrorReason reason() const noexcept;

private:
    DevicePortErrorReason reason_;
};

std::string resolveDevicePort(
    const std::optional<std::string>& explicit_port,
    const std::optional<std::string>& device_id = std::nullopt
);

} // namespace headmotion::app
