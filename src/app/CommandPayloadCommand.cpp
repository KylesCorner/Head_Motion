#include "headmotion/protocol/UsbFrameCodec.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/util/Hex.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace headmotion::app {

int runCommandPayloadCommand(const std::string& port_name, const std::string& payload_hex) {
    using namespace std::chrono_literals;

    const auto payload = headmotion::util::parseHexBytes(payload_hex);
    const auto tx = headmotion::protocol::UsbFrameCodec::encodePayload(payload);

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

    std::cout << "Payload TX [" << payload.size() << " bytes]: "
              << headmotion::util::hexDump(payload)
              << "\n";

    std::cout << "Framed TX [" << tx.size() << " bytes]: "
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

    std::cout << "Raw RX [" << rx.size() << " bytes]:\n";
    std::cout << headmotion::util::hexDump(rx) << "\n";

    const auto frames = headmotion::protocol::UsbFrameCodec::decodeFrames(rx);

    if (frames.empty()) {
        std::cout << "No decoded USB frames.\n";
        return 0;
    }

    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::cout << "Frame " << i << " payload ["
                  << frames[i].payload.size()
                  << " bytes]: "
                  << headmotion::util::hexDump(frames[i].payload)
                  << "\n";
    }

    return 0;
}

} // namespace headmotion::app