#include "headmotion/protocol/UsbFrameCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace headmotion::protocol {

namespace {

constexpr std::uint8_t FRAME_START = 0x1F;
constexpr std::uint8_t FRAME_END = 0x0A;

} // namespace

std::vector<std::uint8_t> UsbFrameCodec::encodePayload(
    const std::vector<std::uint8_t>& payload
) {
    if (payload.size() > 255) {
        throw std::runtime_error("USB payload too large");
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 3);

    frame.push_back(FRAME_START);
    frame.push_back(static_cast<std::uint8_t>(payload.size()));

    frame.insert(frame.end(), payload.begin(), payload.end());

    frame.push_back(FRAME_END);

    return frame;
}

UsbFrameCodec::DecodeResult UsbFrameCodec::decodeFramesWithConsumption(
    const std::vector<std::uint8_t>& bytes
) {
    DecodeResult result;

    std::size_t i = 0;

    while (i < bytes.size()) {
        /*
         * Resynchronize to the next frame start byte.
         * Bytes before 0x1F cannot be part of a valid frame.
         */
        if (bytes[i] != FRAME_START) {
            ++i;
            result.consumed_bytes = i;
            ++result.dropped_bytes;
            continue;
        }

        /*
         * Need at least:
         *
         *   0x1F length 0x0A
         *
         * If we only have the start byte, or start + length, keep those bytes
         * in the caller's RX buffer for the next serial read.
         */
        if (bytes.size() - i < 3) {
            break;
        }

        const std::size_t payload_len =
            static_cast<std::size_t>(bytes[i + 1]);

        const std::size_t frame_len =
            payload_len + 3;

        /*
         * Full frame has not arrived yet. Do not consume the partial frame.
         */
        if (bytes.size() - i < frame_len) {
            break;
        }

        const std::uint8_t terminator =
            bytes[i + frame_len - 1];

        /*
         * Bad terminator means this 0x1F was probably not a real frame start,
         * or the stream is corrupted. Drop only this start byte and resync.
         */
        if (terminator != FRAME_END) {
            ++i;
            result.consumed_bytes = i;
            ++result.dropped_bytes;
            continue;
        }

        UsbFrame frame;

        frame.payload.insert(
            frame.payload.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(i + 2),
            bytes.begin() + static_cast<std::ptrdiff_t>(i + 2 + payload_len)
        );

        result.frames.push_back(std::move(frame));

        i += frame_len;
        result.consumed_bytes = i;
    }

    return result;
}

std::vector<UsbFrame> UsbFrameCodec::decodeFrames(
    const std::vector<std::uint8_t>& bytes
) {
    return decodeFramesWithConsumption(bytes).frames;
}

} // namespace headmotion::protocol