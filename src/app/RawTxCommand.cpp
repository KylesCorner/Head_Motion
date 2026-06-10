#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/util/Hex.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace headmotion::app {

int runRawTxCommand(const std::string& port_name, const std::string& hex_string) {
    using namespace std::chrono_literals;

    const auto tx = headmotion::util::parseHexBytes(hex_string);

    if (tx.empty()) {
        std::cerr << "No bytes to send.\n";
        return 1;
    }

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    auto port = headmotion::transport::SerialPortFactory::create(config);

    std::cout << "Opening " << port_name << "\n";
    port->open();

    std::cout << "TX [" << tx.size() << " bytes]: "
              << headmotion::util::hexDump(tx)
              << "\n";

    port->write(tx);

    std::vector<std::uint8_t> rx;
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;

    while (std::chrono::steady_clock::now() < deadline) {
        auto chunk = port->read(512, 200ms);

        if (!chunk.empty()) {
            rx.insert(rx.end(), chunk.begin(), chunk.end());
        }
    }

    if (rx.empty()) {
        std::cout << "RX: no response\n";
        return 2;
    }

    std::cout << "RX [" << rx.size() << " bytes] hex:\n";
    std::cout << headmotion::util::hexDump(rx) << "\n";

    std::cout << "RX ASCII preview:\n";
    std::cout << headmotion::util::asciiPreview(rx) << "\n";

    return 0;
}

} // namespace headmotion::app