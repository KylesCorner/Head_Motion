#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/sdk/MetaWearSdkBridge.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace headmotion::app {

int runSdkProbeCommand(
    const std::string& port_name
) {
    using namespace std::chrono_literals;

    /*
     * Resolve the physical MMS+ identity.
     *
     * This uses the USB serial when available, or the Linux
     * /dev/serial/by-id path fallback we added earlier.
     */
    const std::string device_id =
        headmotion::session::BoardStateStore::
            deviceIdForPort(port_name);

    std::cout
        << "MMS+ device ID: "
        << device_id
        << "\n";

    std::cout
        << "Current port: "
        << port_name
        << "\n";

    headmotion::transport::SerialConfig config;

    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    auto serial =
        headmotion::transport::SerialPortFactory::
            create(config);

    headmotion::metawear::MetaWearUsbTransport usb(
        *serial
    );

    std::cout
        << "Opening "
        << port_name
        << "\n";

    usb.open();

    headmotion::sdk::MetaWearSdkBridge bridge(
        usb
    );

    std::cout
        << "Initializing MetaWear SDK board\n";

    const bool ok =
        bridge.initialize(5000);

    std::cout
        << "SDK probe initialized="
        << (ok ? "true" : "false")
        << " status="
        << bridge.initializeStatus()
        << "\n";

    if (!ok) {
        std::cerr
            << "SDK probe failed for MMS+ "
            << device_id
            << "\n";

        return 2;
    }

    std::cout
        << "SDK probe successful.\n";

    std::cout
        << "Device ID: "
        << device_id
        << "\n";

    return 0;
}

} // namespace headmotion::app