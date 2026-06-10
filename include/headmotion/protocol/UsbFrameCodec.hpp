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
};

} // namespace headmotion::protocol