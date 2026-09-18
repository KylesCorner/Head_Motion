/*
 * IdentifyCommand
 * ---------------
 *
 * Performs a live identity query against the MMS+ USB serial interface.
 */

#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::app {

int runIdentifyCommand(
    const std::string& port_name,
    CommandOutput& output
) {
    using namespace std::chrono_literals;

    output.started({{"port", port_name}});

    headmotion::transport::SerialConfig config;
    config.port_name = port_name;
    config.baud_rate = 115200;
    config.data_bits = 8;
    config.stop_bits = 1;
    config.assert_dtr = true;
    config.assert_rts = true;
    config.open_delay = 100ms;

    output.status("opening_port");

    auto port =
        headmotion::transport::SerialPortFactory::
        create(config);

    port->open();

    output.status("sending_identity_query");

    const std::vector<std::uint8_t> query = {
        static_cast<std::uint8_t>('?'),
        static_cast<std::uint8_t>('\n')
    };

    port->write(query);

    std::vector<std::uint8_t> response;

    const auto deadline =
        std::chrono::steady_clock::now() +
        1500ms;

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        auto chunk =
            port->read(
                256,
                200ms
            );

        if (chunk.empty()) {
            continue;
        }

        response.insert(
            response.end(),
            chunk.begin(),
            chunk.end()
        );

        const std::string text(
            response.begin(),
            response.end()
        );

        if (
            text.find('\n') != std::string::npos ||
            text.find("MbientLab") != std::string::npos ||
            text.find("MetaMotionS") != std::string::npos
        ) {
            break;
        }
    }

    if (response.empty()) {
        output.error(
            "no_identity_response",
            "No identity response received"
        );

        output.completed(false, 2);
        return 2;
    }

    std::string text(
        response.begin(),
        response.end()
    );

    while (
        !text.empty() &&
        (
            text.back() == '\n' ||
            text.back() == '\r'
        )
    ) {
        text.pop_back();
    }

    output.event(
        "identity",
        {
            {"port", port_name},
            {"identity", text}
        }
    );

    output.completed(true, 0);
    return 0;
}

int runIdentifyCommand(
    const std::string& port_name
) {
    CommandOutput output("identify");
    return runIdentifyCommand(
        port_name,
        output
    );
}

} // namespace headmotion::app
