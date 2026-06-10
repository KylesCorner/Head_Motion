#include "headmotion/protocol/UsbFrameCodec.hpp"

#include <stdexcept>

namespace headmotion::protocol {

std::vector<std::uint8_t> UsbFrameCodec::encodePayload(
    const std::vector<std::uint8_t>& payload
) {
    if (payload.size() > 255) {
        throw std::runtime_error("USB payload too large");
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 3);

    frame.push_back(0x1F);
    frame.push_back(static_cast<std::uint8_t>(payload.size()));

    frame.insert(frame.end(), payload.begin(), payload.end());

    frame.push_back(0x0A);

    return frame;
}

std::vector<UsbFrame> UsbFrameCodec::decodeFrames(
    const std::vector<std::uint8_t>& bytes
) {
    std::vector<UsbFrame> frames;

    std::size_t i = 0;

    while (i < bytes.size()) {
        if (bytes[i] != 0x1F) {
            ++i;
            continue;
        }

        if (i + 2 > bytes.size()) {
            break;
        }

        const auto payload_len = static_cast<std::size_t>(bytes[i + 1]);
        const auto frame_len = payload_len + 3;

        if (i + frame_len > bytes.size()) {
            break;
        }

        const auto terminator = bytes[i + frame_len - 1];

        if (terminator != 0x0A) {
            ++i;
            continue;
        }

        UsbFrame frame;
        frame.payload.insert(
            frame.payload.end(),
            bytes.begin() + static_cast<long>(i + 2),
            bytes.begin() + static_cast<long>(i + 2 + payload_len)
        );

        frames.push_back(std::move(frame));

        i += frame_len;
    }

    return frames;
}

} // namespace headmotion::protocol