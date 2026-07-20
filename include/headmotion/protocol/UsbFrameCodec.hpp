#pragma once

#include <cstdint>
#include <vector>

namespace headmotion::protocol {

struct UsbFrame {
    std::vector<std::uint8_t> payload;
};

class UsbFrameCodec {
public:
    static std::vector<std::uint8_t> encodePayload(
        const std::vector<std::uint8_t>& payload
    );

    static std::vector<UsbFrame> decodeFrames(
        const std::vector<std::uint8_t>& bytes
    );
    struct DecodeResult {
    std::vector<UsbFrame> frames;
    std::size_t consumed_bytes = 0;
    std::size_t dropped_bytes = 0;
    };

    static DecodeResult decodeFramesWithConsumption(
        const std::vector<std::uint8_t>& bytes
    );
};

} // namespace headmotion::protocol