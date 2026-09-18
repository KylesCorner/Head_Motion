#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/protocol/UsbFrameCodec.hpp"
#include "headmotion/session/BoardStateStore.hpp"
#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"
#include "headmotion/util/Hex.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::app {

int runCommandPayloadCommand(
    const std::string& port_name,
    const std::string& payload_hex,
    CommandOutput& output
) {
    using namespace std::chrono_literals;

    const auto payload =
        headmotion::util::parseHexBytes(
            payload_hex
        );

    if (payload.empty()) {
        output.error(
            "empty_payload",
            "No payload bytes to send"
        );

        output.completed(false, 1);
        return 1;
    }

    const auto tx =
        headmotion::protocol::UsbFrameCodec::
            encodePayload(payload);

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
                "payload_bytes",
                static_cast<std::uint64_t>(
                    payload.size()
                )
            },
            {
                "payload_hex",
                headmotion::util::hexDump(
                    payload
                )
            },
            {
                "frame_bytes",
                static_cast<std::uint64_t>(
                    tx.size()
                )
            },
            {
                "frame_hex",
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
        "raw_rx",
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
            }
        }
    );

    const auto frames =
        headmotion::protocol::UsbFrameCodec::
            decodeFrames(rx);

    for (
        std::size_t i = 0;
        i < frames.size();
        ++i
    ) {
        output.event(
            "frame",
            {
                {
                    "index",
                    static_cast<std::uint64_t>(i)
                },
                {
                    "payload_bytes",
                    static_cast<std::uint64_t>(
                        frames[i].payload.size()
                    )
                },
                {
                    "payload_hex",
                    headmotion::util::hexDump(
                        frames[i].payload
                    )
                }
            }
        );
    }

    output.event(
        "summary",
        {
            {
                "decoded_frames",
                static_cast<std::uint64_t>(
                    frames.size()
                )
            }
        }
    );

    output.completed(true, 0);
    return 0;
}

int runCommandPayloadCommand(
    const std::string& port_name,
    const std::string& payload_hex
) {
    CommandOutput output("cmd");

    return runCommandPayloadCommand(
        port_name,
        payload_hex,
        output
    );
}

} // namespace headmotion::app
