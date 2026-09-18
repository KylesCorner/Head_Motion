#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/util/Hex.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::app {

int runModuleInfoCommand(
    const std::string& port_name,
    CommandOutput& output
) {
    using namespace std::chrono_literals;

    const std::string device_id =
        headmotion::session::BoardStateStore::
            deviceIdForPort(port_name);

    output.started(
        {
            {"port", port_name},
            {"device_id", device_id}
        }
    );

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    output.status("opening_port");

    auto serial =
        headmotion::transport::SerialPortFactory::
            create(config);

    headmotion::metawear::MetaWearUsbTransport usb(
        *serial
    );

    usb.open();

    const std::vector<std::uint8_t>
        module_info_payload = {
            0x01,
            0x80
        };

    output.event(
        "tx",
        {
            {
                "payload_hex",
                headmotion::util::hexDump(
                    module_info_payload
                )
            }
        }
    );

    const auto response =
        usb.transactPayload(
            module_info_payload,
            1500ms
        );

    if (response.empty()) {
        output.error(
            "no_module_info_response",
            "No module-info response received"
        );

        output.completed(false, 2);
        return 2;
    }

    const bool shape_ok =
        response.size() >= 4 &&
        response[0] == 0x01 &&
        response[1] == 0x80;

    output.event(
        "module_info",
        {
            {
                "bytes",
                static_cast<std::uint64_t>(
                    response.size()
                )
            },
            {
                "payload_hex",
                headmotion::util::hexDump(
                    response
                )
            },
            {
                "expected_shape",
                shape_ok
            }
        }
    );

    output.completed(true, 0);
    return 0;
}

int runModuleInfoCommand(
    const std::string& port_name
) {
    CommandOutput output("module-info");

    return runModuleInfoCommand(
        port_name,
        output
    );
}

} // namespace headmotion::app
