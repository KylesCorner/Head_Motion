#include "headmotion/metawear/MetaWearUsbTransport.hpp"

#include <chrono>
#include <stdexcept>
#include <vector>

namespace headmotion::metawear {

MetaWearUsbTransport::MetaWearUsbTransport(
    headmotion::transport::IByteTransport& byte_transport
)
    : byte_transport_(byte_transport) {}

void MetaWearUsbTransport::open() {
    byte_transport_.open();
}

void MetaWearUsbTransport::close() {
    byte_transport_.close();
}

bool MetaWearUsbTransport::isOpen() const {
    return byte_transport_.isOpen();
}

void MetaWearUsbTransport::writePayload(const std::vector<std::uint8_t>& payload) {
    const auto frame = headmotion::protocol::UsbFrameCodec::encodePayload(payload);
    byte_transport_.write(frame);
}

std::vector<std::uint8_t> MetaWearUsbTransport::transactPayload(
    const std::vector<std::uint8_t>& payload,
    std::chrono::milliseconds timeout
) {
    writePayload(payload);

    const auto frames = readFrames(timeout);

    if (frames.empty()) {
        return {};
    }

    return frames.front().payload;
}

std::vector<headmotion::protocol::UsbFrame> MetaWearUsbTransport::readFrames(
    std::chrono::milliseconds timeout
) {
    const auto raw = readRawUntilQuiet(
        timeout,
        std::chrono::milliseconds{100},
        1048576
    );

    if (raw.empty()) {
        return {};
    }

    return headmotion::protocol::UsbFrameCodec::decodeFrames(raw);
}

std::vector<std::uint8_t> MetaWearUsbTransport::readRawUntilQuiet(
    std::chrono::milliseconds total_timeout,
    std::chrono::milliseconds quiet_timeout,
    std::size_t max_bytes
) {
    std::vector<std::uint8_t> out;

    const auto start = std::chrono::steady_clock::now();
    auto last_rx = start;

    while (true) {
        const auto now = std::chrono::steady_clock::now();

        if (now - start >= total_timeout) {
            break;
        }

        if (!out.empty() && now - last_rx >= quiet_timeout) {
            break;
        }

        const auto remaining = total_timeout - std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        const auto read_timeout = remaining < quiet_timeout ? remaining : quiet_timeout;

        auto chunk = byte_transport_.read(512, read_timeout);

        if (!chunk.empty()) {
            if (out.size() + chunk.size() > max_bytes) {
                throw std::runtime_error("MetaWear USB read exceeded max buffer size");
            }

            out.insert(out.end(), chunk.begin(), chunk.end());
            last_rx = std::chrono::steady_clock::now();
        }
    }

    return out;
}

} // namespace headmotion::metawear