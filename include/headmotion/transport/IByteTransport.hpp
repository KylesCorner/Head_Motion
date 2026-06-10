#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace headmotion::transport {

class IByteTransport {
public:
    virtual ~IByteTransport() = default;

    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual void write(const std::vector<std::uint8_t>& bytes) = 0;

    virtual std::vector<std::uint8_t> read(
        std::size_t max_bytes,
        std::chrono::milliseconds timeout
    ) = 0;
};

} // namespace headmotion::transport