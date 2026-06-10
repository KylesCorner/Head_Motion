#pragma once

#include "headmotion/protocol/UsbFrameCodec.hpp"
#include "headmotion/transport/IByteTransport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace headmotion::metawear {

class MetaWearUsbTransport {
public:
    explicit MetaWearUsbTransport(headmotion::transport::IByteTransport& byte_transport);

    void open();
    void close();
    bool isOpen() const;

    void writePayload(const std::vector<std::uint8_t>& payload);

    std::vector<std::uint8_t> transactPayload(
        const std::vector<std::uint8_t>& payload,
        std::chrono::milliseconds timeout
    );

    std::vector<headmotion::protocol::UsbFrame> readFrames(
        std::chrono::milliseconds timeout
    );

private:
    headmotion::transport::IByteTransport& byte_transport_;

    std::vector<std::uint8_t> readRawUntilQuiet(
        std::chrono::milliseconds total_timeout,
        std::chrono::milliseconds quiet_timeout,
        std::size_t max_bytes
    );
};

} // namespace headmotion::metawear