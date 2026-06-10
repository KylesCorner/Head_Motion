#include "headmotion/metawear/MetaWearUsbTransport.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/util/Hex.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace headmotion::app {

int runModuleInfoCommand(const std::string& port_name) {
    using namespace std::chrono_literals;

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    auto serial = headmotion::transport::SerialPortFactory::create(config);
    headmotion::metawear::MetaWearUsbTransport usb(*serial);

    std::cout << "Opening " << port_name << "\n";
    usb.open();

    const std::vector<std::uint8_t> module_info_payload = {
        0x01,
        0x80
    };

    std::cout << "Sending module-info payload: "
              << headmotion::util::hexDump(module_info_payload)
              << "\n";

    const auto response = usb.transactPayload(module_info_payload, 1500ms);

    if (response.empty()) {
        std::cout << "No module-info response received.\n";
        return 2;
    }

    std::cout << "Module-info response payload ["
              << response.size()
              << " bytes]: "
              << headmotion::util::hexDump(response)
              << "\n";

    if (response.size() >= 4 &&
        response[0] == 0x01 &&
        response[1] == 0x80) {
        std::cout << "Module-info round trip OK.\n";
    } else {
        std::cout << "Unexpected module-info response shape.\n";
    }

    return 0;
}

} // namespace headmotion::app