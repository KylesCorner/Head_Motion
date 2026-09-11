/*
 * IdentifyCommand
 * ---------------
 *
 * Performs a live identity query against the MMS+ USB serial interface.
 *
 * Stateless host behavior:
 *
 *   - Opens only the requested serial port.
 *   - Sends the MMS+ identity query.
 *   - Prints the live response from the device.
 *   - Does not create or persist a host-side device ID.
 *   - Does not read or write BoardStateStore / DevicePortStore state.
 */

#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace headmotion::app {

    int runIdentifyCommand(
        const std::string& port_name
    ) {
        using namespace std::chrono_literals;

        headmotion::transport::SerialConfig config;
        config.port_name = port_name;
        config.baud_rate = 115200;
        config.data_bits = 8;
        config.stop_bits = 1;
        config.assert_dtr = true;
        config.assert_rts = true;
        config.open_delay = 100ms;

        auto port =
            headmotion::transport::SerialPortFactory::create(
                config
            );

        std::cout
            << "Opening "
            << port_name
            << "\n";

        port->open();

        std::cout
            << "Sending identity query: ?\\n\n";

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
            std::cout
                << "No response received from "
                << port_name
                << ".\n";

            return 2;
        }

        std::string text(
            response.begin(),
            response.end()
        );

        /*
         * Remove trailing CR/LF so the identity prints cleanly.
         */
        while (
            !text.empty() &&
            (
                text.back() == '\n' ||
                text.back() == '\r'
                )
            ) {
            text.pop_back();
        }

        std::cout
            << "Response: "
            << text
            << "\n";

        std::cout
            << "Current port: "
            << port_name
            << "\n";

        return 0;
    }

} // namespace headmotion::app
