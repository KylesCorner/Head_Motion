#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <chrono>
#include <string>

namespace headmotion::app {

int runSdkProbeCommand(
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

    headmotion::sdk::MetaWearSdkBridge bridge(
        usb
    );

    output.status("initializing_sdk");

    const bool ok =
        bridge.initialize(5000);

    output.event(
        "sdk_probe",
        {
            {"initialized", ok},
            {
                "status",
                bridge.initializeStatus()
            }
        }
    );

    if (!ok) {
        output.error(
            "sdk_probe_failed",
            "SDK probe failed"
        );

        output.completed(false, 2);
        return 2;
    }

    output.completed(true, 0);
    return 0;
}

int runSdkProbeCommand(
    const std::string& port_name
) {
    CommandOutput output("sdk-probe");

    return runSdkProbeCommand(
        port_name,
        output
    );
}

} // namespace headmotion::app
