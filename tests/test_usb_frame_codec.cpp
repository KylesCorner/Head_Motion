/*
 * UsbFrameCodec unit tests.
 *
 * No hardware required.
 *
 * Covers:
 *   - normal encode
 *   - normal decode
 *   - empty payload
 *   - maximum 255-byte payload
 *   - oversized payload rejection
 *   - partial frame
 *   - frame split across multiple reads
 *   - multiple frames in one read
 *   - garbage before frame
 *   - bad terminator / resynchronization
 *   - declared length larger than available data
 *   - payload containing framing bytes
 *   - consumption accounting
 */

#include "headmotion/protocol/UsbFrameCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

using headmotion::protocol::UsbFrame;
using headmotion::protocol::UsbFrameCodec;

int failures = 0;


void fail(
    const std::string& test,
    const std::string& message
) {
    ++failures;

    std::cerr
        << "FAIL: "
        << test
        << ": "
        << message
        << '\n';
}


void pass(
    const std::string& test
) {
    std::cout
        << "PASS: "
        << test
        << '\n';
}


template<typename T>
void expectEqual(
    const std::string& test,
    const T& actual,
    const T& expected,
    const std::string& field
) {
    if (actual != expected) {
        fail(
            test,
            field + " did not match"
        );
    }
}


void expectSize(
    const std::string& test,
    std::size_t actual,
    std::size_t expected,
    const std::string& field
) {
    if (actual != expected) {
        fail(
            test,
            field +
            ": expected " +
            std::to_string(expected) +
            ", got " +
            std::to_string(actual)
        );
    }
}


/*
 * ---------------------------------------------------------
 * Normal encode
 * ---------------------------------------------------------
 */

void testEncodeNormal()
{
    constexpr const char* TEST =
        "encode normal frame";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t> payload{
        0x01,
        0x02,
        0x03
    };

    const std::vector<std::uint8_t> expected{
        0x1F,
        0x03,
        0x01,
        0x02,
        0x03,
        0x0A
    };

    const auto encoded =
        UsbFrameCodec::encodePayload(
            payload
        );

    expectEqual(
        TEST,
        encoded,
        expected,
        "encoded frame"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Normal decode
 * ---------------------------------------------------------
 */

void testDecodeNormal()
{
    constexpr const char* TEST =
        "decode normal frame";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t> bytes{
        0x1F,
        0x03,
        0xAA,
        0xBB,
        0xCC,
        0x0A
    };

    const auto frames =
        UsbFrameCodec::decodeFrames(
            bytes
        );

    expectSize(
        TEST,
        frames.size(),
        1,
        "frame count"
    );

    if (!frames.empty()) {
        expectEqual(
            TEST,
            frames[0].payload,
            std::vector<std::uint8_t>{
                0xAA,
                0xBB,
                0xCC
            },
            "payload"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Empty payload
 * ---------------------------------------------------------
 */

void testEmptyPayload()
{
    constexpr const char* TEST =
        "empty payload";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t> payload;

    const auto encoded =
        UsbFrameCodec::encodePayload(
            payload
        );

    const std::vector<std::uint8_t> expected{
        0x1F,
        0x00,
        0x0A
    };

    expectEqual(
        TEST,
        encoded,
        expected,
        "encoded empty frame"
    );

    const auto decoded =
        UsbFrameCodec::decodeFrames(
            encoded
        );

    expectSize(
        TEST,
        decoded.size(),
        1,
        "decoded frame count"
    );

    if (!decoded.empty()) {
        expectSize(
            TEST,
            decoded[0].payload.size(),
            0,
            "decoded payload size"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Maximum legal payload
 * ---------------------------------------------------------
 */

void testMaximumPayload()
{
    constexpr const char* TEST =
        "maximum 255-byte payload";

    const int failures_before =
        failures;

    std::vector<std::uint8_t> payload(
        255
    );

    for (
        std::size_t i = 0;
        i < payload.size();
        ++i
    ) {
        payload[i] =
            static_cast<std::uint8_t>(
                i
            );
    }

    const auto encoded =
        UsbFrameCodec::encodePayload(
            payload
        );

    /*
     * start + length + 255 payload + end
     */
    expectSize(
        TEST,
        encoded.size(),
        258,
        "encoded size"
    );

    if (encoded.size() >= 2) {
        if (encoded[0] != 0x1F) {
            fail(
                TEST,
                "missing frame start"
            );
        }

        if (encoded[1] != 0xFF) {
            fail(
                TEST,
                "length byte was not 255"
            );
        }

        if (encoded.back() != 0x0A) {
            fail(
                TEST,
                "missing frame terminator"
            );
        }
    }

    const auto decoded =
        UsbFrameCodec::decodeFrames(
            encoded
        );

    expectSize(
        TEST,
        decoded.size(),
        1,
        "decoded frame count"
    );

    if (!decoded.empty()) {
        expectEqual(
            TEST,
            decoded[0].payload,
            payload,
            "decoded payload"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Payload too large
 * ---------------------------------------------------------
 */

void testOversizedPayload()
{
    constexpr const char* TEST =
        "reject 256-byte payload";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t>
        payload(
            256,
            0xAA
        );

    bool threw =
        false;

    try {
        (void)
            UsbFrameCodec::encodePayload(
                payload
            );
    }
    catch (const std::exception&) {
        threw = true;
    }

    if (!threw) {
        fail(
            TEST,
            "256-byte payload was accepted"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Partial frame
 * ---------------------------------------------------------
 */

void testPartialFrame()
{
    constexpr const char* TEST =
        "partial frame retained";

    const int failures_before =
        failures;

    /*
     * Declares 3 payload bytes, but only 2 have arrived.
     */
    const std::vector<std::uint8_t> bytes{
        0x1F,
        0x03,
        0xAA,
        0xBB
    };

    const auto result =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                bytes
            );

    expectSize(
        TEST,
        result.frames.size(),
        0,
        "frame count"
    );

    /*
     * A partial frame must remain in the caller's RX buffer.
     */
    expectSize(
        TEST,
        result.consumed_bytes,
        0,
        "consumed bytes"
    );

    expectSize(
        TEST,
        result.dropped_bytes,
        0,
        "dropped bytes"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Split frame across serial reads
 * ---------------------------------------------------------
 */

void testFrameSplitAcrossReads()
{
    constexpr const char* TEST =
        "frame split across reads";

    const int failures_before =
        failures;

    std::vector<std::uint8_t> rx_buffer{
        0x1F,
        0x04,
        0x11
    };

    /*
     * First serial read does not contain the whole frame.
     */
    auto first =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                rx_buffer
            );

    expectSize(
        TEST,
        first.frames.size(),
        0,
        "first read frame count"
    );

    expectSize(
        TEST,
        first.consumed_bytes,
        0,
        "first read consumed bytes"
    );

    /*
     * Simulate the next serial read being appended to the existing
     * RX buffer.
     */
    const std::vector<std::uint8_t> second_read{
        0x22,
        0x33,
        0x44,
        0x0A
    };

    rx_buffer.insert(
        rx_buffer.end(),
        second_read.begin(),
        second_read.end()
    );

    auto second =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                rx_buffer
            );

    expectSize(
        TEST,
        second.frames.size(),
        1,
        "second read frame count"
    );

    if (!second.frames.empty()) {
        expectEqual(
            TEST,
            second.frames[0].payload,
            std::vector<std::uint8_t>{
                0x11,
                0x22,
                0x33,
                0x44
            },
            "reassembled payload"
        );
    }

    expectSize(
        TEST,
        second.consumed_bytes,
        7,
        "consumed bytes"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Two frames in one serial read
 * ---------------------------------------------------------
 */

void testMultipleFrames()
{
    constexpr const char* TEST =
        "two frames in one read";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t> bytes{
        0x1F,
        0x02,
        0xAA,
        0xBB,
        0x0A,

        0x1F,
        0x03,
        0x01,
        0x02,
        0x03,
        0x0A
    };

    const auto result =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                bytes
            );

    expectSize(
        TEST,
        result.frames.size(),
        2,
        "frame count"
    );

    if (result.frames.size() == 2) {
        expectEqual(
            TEST,
            result.frames[0].payload,
            std::vector<std::uint8_t>{
                0xAA,
                0xBB
            },
            "first payload"
        );

        expectEqual(
            TEST,
            result.frames[1].payload,
            std::vector<std::uint8_t>{
                0x01,
                0x02,
                0x03
            },
            "second payload"
        );
    }

    expectSize(
        TEST,
        result.consumed_bytes,
        bytes.size(),
        "consumed bytes"
    );

    expectSize(
        TEST,
        result.dropped_bytes,
        0,
        "dropped bytes"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Garbage before a frame
 * ---------------------------------------------------------
 */

void testGarbageBeforeFrame()
{
    constexpr const char* TEST =
        "garbage before valid frame";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t> bytes{
        0xDE,
        0xAD,
        0xBE,
        0xEF,

        0x1F,
        0x02,
        0x12,
        0x34,
        0x0A
    };

    const auto result =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                bytes
            );

    expectSize(
        TEST,
        result.frames.size(),
        1,
        "frame count"
    );

    expectSize(
        TEST,
        result.dropped_bytes,
        4,
        "dropped garbage bytes"
    );

    expectSize(
        TEST,
        result.consumed_bytes,
        bytes.size(),
        "consumed bytes"
    );

    if (!result.frames.empty()) {
        expectEqual(
            TEST,
            result.frames[0].payload,
            std::vector<std::uint8_t>{
                0x12,
                0x34
            },
            "payload"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Corrupted frame followed by valid frame
 * ---------------------------------------------------------
 */

void testBadTerminatorResync()
{
    constexpr const char* TEST =
        "bad terminator resynchronizes";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t> bytes{
        /*
         * Corrupted frame:
         *
         * 0x1F
         * length = 2
         * payload = AA BB
         * terminator should be 0A, but is FF.
         */
        0x1F,
        0x02,
        0xAA,
        0xBB,
        0xFF,

        /*
         * Valid frame follows.
         */
        0x1F,
        0x02,
        0x11,
        0x22,
        0x0A
    };

    const auto result =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                bytes
            );

    expectSize(
        TEST,
        result.frames.size(),
        1,
        "frame count"
    );

    /*
     * The decoder drops the false 0x1F and then walks over the
     * corrupted bytes until it finds the next frame start.
     */
    expectSize(
        TEST,
        result.dropped_bytes,
        5,
        "dropped corrupt bytes"
    );

    if (!result.frames.empty()) {
        expectEqual(
            TEST,
            result.frames[0].payload,
            std::vector<std::uint8_t>{
                0x11,
                0x22
            },
            "recovered payload"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Declared length larger than available bytes
 * ---------------------------------------------------------
 */

void testIncompleteDeclaredLength()
{
    constexpr const char* TEST =
        "declared length larger than available data";

    const int failures_before =
        failures;

    /*
     * Claims a 10-byte payload, but only 3 payload bytes are present.
     *
     * The codec cannot know whether this is corruption or simply a
     * fragmented serial read, so the correct behavior is to retain it
     * and wait for more bytes.
     */
    const std::vector<std::uint8_t> bytes{
        0x1F,
        0x0A,
        0x01,
        0x02,
        0x03
    };

    const auto result =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                bytes
            );

    expectSize(
        TEST,
        result.frames.size(),
        0,
        "frame count"
    );

    expectSize(
        TEST,
        result.consumed_bytes,
        0,
        "consumed bytes"
    );

    expectSize(
        TEST,
        result.dropped_bytes,
        0,
        "dropped bytes"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Framing bytes inside payload
 * ---------------------------------------------------------
 */

void testFramingBytesInsidePayload()
{
    constexpr const char* TEST =
        "framing bytes inside payload";

    const int failures_before =
        failures;

    /*
     * A payload may legitimately contain 0x1F or 0x0A.
     * Length framing means these must not confuse the decoder.
     */
    const std::vector<std::uint8_t> payload{
        0xAA,
        0x1F,
        0xBB,
        0x0A,
        0xCC
    };

    const auto encoded =
        UsbFrameCodec::encodePayload(
            payload
        );

    const auto result =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                encoded
            );

    expectSize(
        TEST,
        result.frames.size(),
        1,
        "frame count"
    );

    if (!result.frames.empty()) {
        expectEqual(
            TEST,
            result.frames[0].payload,
            payload,
            "payload"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Valid frame followed by partial frame
 * ---------------------------------------------------------
 */

void testCompleteThenPartial()
{
    constexpr const char* TEST =
        "complete frame followed by partial frame";

    const int failures_before =
        failures;

    const std::vector<std::uint8_t> bytes{
        /*
         * Complete frame: 5 bytes.
         */
        0x1F,
        0x02,
        0xAA,
        0xBB,
        0x0A,

        /*
         * Start of another frame.
         */
        0x1F,
        0x03,
        0x11
    };

    const auto result =
        UsbFrameCodec::
            decodeFramesWithConsumption(
                bytes
            );

    expectSize(
        TEST,
        result.frames.size(),
        1,
        "frame count"
    );

    /*
     * Only the first complete frame should be consumed.
     * The partial second frame remains in the RX buffer.
     */
    expectSize(
        TEST,
        result.consumed_bytes,
        5,
        "consumed bytes"
    );

    expectSize(
        TEST,
        result.dropped_bytes,
        0,
        "dropped bytes"
    );

    if (!result.frames.empty()) {
        expectEqual(
            TEST,
            result.frames[0].payload,
            std::vector<std::uint8_t>{
                0xAA,
                0xBB
            },
            "payload"
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


int runTests()
{
    testEncodeNormal();
    testDecodeNormal();

    testEmptyPayload();

    testMaximumPayload();
    testOversizedPayload();

    testPartialFrame();
    testFrameSplitAcrossReads();

    testMultipleFrames();

    testGarbageBeforeFrame();
    testBadTerminatorResync();

    testIncompleteDeclaredLength();

    testFramingBytesInsidePayload();

    testCompleteThenPartial();

    return failures;
}

} // namespace


int main()
{
    std::cout
        << "========================================\n"
        << "HeadMotion USB Frame Codec Tests\n"
        << "========================================\n\n";

    const int result =
        runTests();

    if (result != 0) {
        std::cerr
            << "\n"
            << result
            << " USB frame codec test(s) FAILED\n";

        return 1;
    }

    std::cout
        << "\nAll USB frame codec tests PASSED\n";

    return 0;
}