#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/util/Hex.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::app {

int runRawTxCommand(
    const std::string& port_name,
    const std::string& hex_string,
    CommandOutput& output
) {
    using namespace std::chrono_literals;

    const auto tx =
        headmotion::util::parseHexBytes(
            hex_string
        );

    if (tx.empty()) {
        output.error(
            "empty_tx",
            "No bytes to send"
        );

        output.completed(false, 1);
        return 1;
    }

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

    auto port =
        headmotion::transport::SerialPortFactory::
        create(config);

    port->open();

    output.event(
        "tx",
        {
            {
                "bytes",
                static_cast<std::uint64_t>(
                    tx.size()
                )
            },
            {
                "hex",
                headmotion::util::hexDump(tx)
            }
        }
    );

    port->write(tx);

    std::vector<std::uint8_t> rx;

    const auto deadline =
        std::chrono::steady_clock::now() +
        1500ms;

    while (
        std::chrono::steady_clock::now() <
        deadline
    ) {
        auto chunk =
            port->read(
                512,
                200ms
            );

        if (!chunk.empty()) {
            rx.insert(
                rx.end(),
                chunk.begin(),
                chunk.end()
            );
        }
    }

    if (rx.empty()) {
        output.error(
            "no_response",
            "No response received"
        );

        output.completed(false, 2);
        return 2;
    }

    output.event(
        "rx",
        {
            {
                "bytes",
                static_cast<std::uint64_t>(
                    rx.size()
                )
            },
            {
                "hex",
                headmotion::util::hexDump(rx)
            },
            {
                "ascii",
                headmotion::util::asciiPreview(rx)
            }
        }
    );

    output.completed(true, 0);
    return 0;
}

int runRawTxCommand(
    const std::string& port_name,
    const std::string& hex_string
) {
    CommandOutput output("tx-raw");

    return runRawTxCommand(
        port_name,
        hex_string,
        output
    );
}

} // namespace headmotion::app
