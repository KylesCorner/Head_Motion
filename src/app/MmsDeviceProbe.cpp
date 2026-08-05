#include "headmotion/app/MmsDeviceProbe.hpp"

#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::app {

std::optional<MmsProbeResult> probeMmsDevice(
    const std::string& port_name
) {
    using namespace std::chrono_literals;

    try {
        headmotion::transport::SerialConfig config;
        config.port_name = port_name;
        config.baud_rate = 115200;
        config.data_bits = 8;
        config.stop_bits = 1;
        config.assert_dtr = true;
        config.assert_rts = true;
        config.open_delay = 100ms;

        auto port =
            headmotion::transport::SerialPortFactory::create(config);

        port->open();

        const std::vector<std::uint8_t> query = {
            static_cast<std::uint8_t>('?'),
            static_cast<std::uint8_t>('\n')
        };

        port->write(query);

        std::vector<std::uint8_t> response;

        const auto deadline =
            std::chrono::steady_clock::now() + 1500ms;

        while (std::chrono::steady_clock::now() < deadline) {
            auto chunk = port->read(256, 200ms);

            response.insert(
                response.end(),
                chunk.begin(),
                chunk.end()
            );

            const std::string text(
                response.begin(),
                response.end()
            );

            if (text.find('\n') != std::string::npos) {
                break;
            }
        }

        if (response.empty()) {
            return std::nullopt;
        }

        const std::string identity(
            response.begin(),
            response.end()
        );

        const bool verified =
            identity.find("MetaMotionS") != std::string::npos ||
            identity.find("MetaWear") != std::string::npos ||
            identity.find("MbientLab") != std::string::npos;

        if (!verified) {
            return std::nullopt;
        }

        return MmsProbeResult{
            .identity = identity
        };
    } catch (...) {
        // Scan should continue when a candidate cannot be opened.
        return std::nullopt;
    }
}

} // namespace headmotion::app