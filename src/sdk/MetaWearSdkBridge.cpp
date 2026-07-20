#include "headmotion/sdk/MetaWearSdkBridge.hpp"

#include "headmotion/util/Hex.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>

namespace headmotion::sdk {

namespace {

std::vector<std::uint8_t> stringBytes(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

} // namespace

MetaWearSdkBridge::MetaWearSdkBridge(headmotion::metawear::MetaWearUsbTransport& usb)
    : usb_(usb) {
    connection_.context = this;
    connection_.write_gatt_char = &MetaWearSdkBridge::writeGattCharThunk;
    connection_.read_gatt_char = &MetaWearSdkBridge::readGattCharThunk;
    connection_.enable_notifications = &MetaWearSdkBridge::enableNotificationsThunk;
    connection_.on_disconnect = &MetaWearSdkBridge::onDisconnectThunk;

    board_ = mbl_mw_metawearboard_create(&connection_);

    if (board_ == nullptr) {
        throw std::runtime_error("mbl_mw_metawearboard_create returned null");
    }
}

MetaWearSdkBridge::~MetaWearSdkBridge() {
    if (board_ != nullptr) {
        mbl_mw_metawearboard_free(board_);
        board_ = nullptr;
    }
}

MblMwMetaWearBoard* MetaWearSdkBridge::board() {
    return board_;
}

MblMwBtleConnection* MetaWearSdkBridge::connection() {
    return &connection_;
}

bool MetaWearSdkBridge::initialize(int timeout_ms) {
    initialized_ = false;
    initialize_status_ = -999;

    std::cout << "SDK: initializing board\n";

    mbl_mw_metawearboard_initialize(
        board_,
        this,
        &MetaWearSdkBridge::initializedThunk
    );

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        pumpOnce(100);

        if (initialized_) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return initialized_ && initialize_status_ == 0;
}

void MetaWearSdkBridge::pumpOnce(int timeout_ms) {
    if (in_pump_) {
        pump_requested_ = true;
        return;
    }

    struct PumpGuard {
        bool& flag;

        explicit PumpGuard(bool& value)
            : flag(value) {
            flag = true;
        }

        ~PumpGuard() {
            flag = false;
        }
    };

    PumpGuard guard(in_pump_);
    static std::uint64_t log_data_packets_seen = 0;

    do {
        pump_requested_ = false;

        const auto frames =
            usb_.readFrames(std::chrono::milliseconds(timeout_ms));

        for (const auto& frame : frames) {
            if (frame.payload.empty()) {
                // std::cerr << "\n[metawear RX empty]\n";
                continue;
            }

            if (frame.payload.size() < 2) {
                // std::cerr
                //     << "\n[metawear RX short] "
                //     << headmotion::util::hexDump(frame.payload)
                //     << "\n";
                continue;
            }

            const bool is_logging_packet =
                frame.payload[0] == 0x0b;

            const bool is_log_data_packet =
                is_logging_packet && frame.payload[1] == 0x07;

            if (is_log_data_packet) {
                ++log_data_packets_seen;

                // if ((log_data_packets_seen % 1000) == 0) {
                //     std::cerr
                //         << "\n[metawear RX log data packets] "
                //         << log_data_packets_seen
                //         << "\n";
                // }
            }
            /*
            * 0B 07 is the high-rate log data stream. Do not print every one.
            * We only care about logging control/status packets now.
            */
            // if (is_logging_packet && !is_log_data_packet) {
            //     std::cerr
            //         << "\n[metawear RX logging control] "
            //         << headmotion::util::hexDump(frame.payload)
            //         << "\n";
            // }

            feedNotificationPayload(frame.payload);
        }
        /*
         * If the SDK wrote a command while processing a notification, do one
         * immediate follow-up read pass after unwinding, rather than recursing.
         */
        timeout_ms = 10;

    } 
    while (pump_requested_);
}

bool MetaWearSdkBridge::initialized() const {
    return initialized_;
}

int MetaWearSdkBridge::initializeStatus() const {
    return initialize_status_;
}

void MetaWearSdkBridge::handleWriteGattChar(
    const void* caller,
    MblMwGattCharWriteType write_type,
    const MblMwGattChar* characteristic,
    const std::uint8_t* value,
    std::uint8_t length
) {
    (void)caller;
    (void)write_type;
    (void)characteristic;

    const std::vector<std::uint8_t> payload(value, value + length);
     /*
     * USB CDC is much faster than the BLE transport the MetaWear SDK was
     * originally designed around. During large log downloads, the SDK can issue
     * page/readout commands back-to-back. Pace outbound commands explicitly
     * instead of relying on terminal/debug logging to slow the loop down.
     */
    constexpr auto MIN_WRITE_SPACING =
        std::chrono::microseconds(600);

    const auto now = std::chrono::steady_clock::now();
        if (last_write_time_ != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = now - last_write_time_;

        if (elapsed < MIN_WRITE_SPACING) {
            std::this_thread::sleep_for(MIN_WRITE_SPACING - elapsed);
        }
    }

    // if (!payload.empty() && payload[0] == 0x0b) {
    //     std::cerr
    //         << "\n[metawear TX logging] "
    //         << headmotion::util::hexDump(payload)
    //         << "\n";
    // }
    usb_.writePayload(payload);

    last_write_time_ = std::chrono::steady_clock::now();

    if (in_pump_) {
        pump_requested_ = true;
        return;
    }

    pumpOnce(150);

    // usb_.writePayload(payload);

    // if (in_pump_) {
    //     pump_requested_ = true;
    //     return;
    // }

    // pumpOnce(150);
}

// void MetaWearSdkBridge::handleWriteGattChar(
//     const void* caller,
//     MblMwGattCharWriteType write_type,
//     const MblMwGattChar* characteristic,
//     const std::uint8_t* value,
//     std::uint8_t length
// ) {
//     (void)caller;
//     (void)characteristic;

//     const std::vector<std::uint8_t> payload(value, value + length);

//     std::cout << "SDK write_gatt_char TX payload ["
//               << payload.size()
//               << " bytes]: "
//               << headmotion::util::hexDump(payload)
//               << "\n";

//     if (!payload.empty() && payload[0] == 0x0b) {
//         std::cerr
//             << "\n[metawear TX logging] "
//             << headmotion::util::hexDump(payload)
//             << "\n";
//     }
//     usb_.writePayload(payload);

//     /*
//      * Some SDK writes produce an immediate module response.
//      * Pull a short response window here and feed it back as if it were
//      * a BLE notification.
//      * 
//      * However, when this write callback is invoked from inside SDK notification
//      * handling, calling pumpOnce() recursively causes stack growth during log
//      * page handshakes. Only do the short response pump when we are not already
//      * inside pumpOnce().
//      */
//     if (!in_pump_) {
//         pumpOnce(150);
//     }
// }

void MetaWearSdkBridge::handleReadGattChar(
    const void* caller,
    const MblMwGattChar* characteristic,
    MblMwFnIntVoidPtrArray handler
) {
    (void)characteristic;

    /*
     * During initialization, the MetaWear SDK may read Device Information
     * Service values over BLE. Over USB we already know these from the
     * identity response:
     *
     *   MbientLab MetaMotionS 8 0.1 1.7.2 0561E1
     *
     * This first probe uses ordered fallback values. If init fails, the
     * next refinement is to inspect the requested characteristic UUID and
     * return the exact matching DIS field.
     */
    static const std::vector<std::string> fallback_dis_values = {
    "1.7.2",        // firmware revision - SDK expects semver first here
    "0.1",          // hardware revision
    "MetaMotionS",  // model number/name
    "MbientLab",    // manufacturer
    "0561E1",       // serial number
    "8"             // model id-ish fallback
    };

    const std::string selected =
        fallback_dis_values[
            static_cast<std::size_t>(dis_read_count_) % fallback_dis_values.size()
        ];

    ++dis_read_count_;

    const auto bytes = stringBytes(selected);

    std::cout << "SDK read_gatt_char fallback response: \""
              << selected
              << "\"\n";

    if (handler != nullptr) {
        handler(caller, bytes.data(), static_cast<std::uint8_t>(bytes.size()));
    }
}

void MetaWearSdkBridge::handleEnableNotifications(
    const void* caller,
    const MblMwGattChar* characteristic,
    MblMwFnIntVoidPtrArray handler,
    MblMwFnVoidVoidPtrInt ready
) {
    (void)characteristic;

    notify_caller_ = caller;
    notify_handler_ = handler;

    std::cout << "SDK enable_notifications registered\n";

    if (ready != nullptr) {
        ready(caller, 0);
    }
}

void MetaWearSdkBridge::handleDisconnectSubscribe(
    const void* caller,
    MblMwFnVoidVoidPtrInt handler
) {
    (void)caller;
    (void)handler;

    std::cout << "SDK on_disconnect registered\n";
}

void MetaWearSdkBridge::feedNotificationPayload(
    const std::vector<std::uint8_t>& payload
) {
    if (payload.empty()) {
        return;
    }

    if (payload.size() < 2) {
        return;
    }

    if (notify_handler_ == nullptr || notify_caller_ == nullptr) {
        std::cout
            << "SDK bridge has no notification handler yet; dropping payload\n";
        return;
    }

    if (payload.size() > 255) {
        std::cerr
            << "SDK bridge: dropping payload larger than SDK callback can accept, len="
            << payload.size()
            << "\n";

        return;
    }

    const bool is_logging_payload =
        payload[0] == 0x0b;

    /*
    * Do NOT drop all macro-module packets.
    *
    * The SDK may query module info during initialization, for example:
    *
    *   0F 80 ...
    *
    * That is legitimate and must reach the SDK.
    *
    * The crash case observed during long sync was a suspicious macro command
    * response:
    *
    *   0F 02 ... len=176
    *
    * This application does not create macros during sync, and a large 0F 02
    * payload is likely stale or misframed traffic.
    */
    if (payload[0] == 0x0f &&
        payload[1] == 0x02 &&
        payload.size() > 8) {

        std::cerr
            << "SDK bridge: dropping suspicious macro response, len="
            << payload.size()
            << " payload="
            << headmotion::util::hexDump(payload)
            << "\n";

        return;
    }

    /*
     * Battery logging is currently disabled. A large timer-create response
     * during sync is suspicious and has previously crashed inside timer_created().
     */
    if (payload[0] == 0x0c &&
        payload[1] == 0x02 &&
        payload.size() > 8) {

        std::cerr
            << "SDK bridge: dropping suspicious timer-create response, len="
            << payload.size()
            << " payload="
            << headmotion::util::hexDump(payload)
            << "\n";

        return;
    }

    /*
     * During sync, large non-logging payloads are suspicious. The valid high-rate
     * stream should be logging data/control traffic. This prevents a bad USB
     * frame boundary from being interpreted as an unrelated SDK module response.
     */
    if (!is_logging_payload && payload.size() > 32) {
        std::cerr
            << "SDK bridge: dropping oversized non-logging payload, len="
            << payload.size()
            << " payload="
            << headmotion::util::hexDump(payload)
            << "\n";

        return;
    }

    notify_handler_(
        notify_caller_,
        payload.data(),
        static_cast<std::uint8_t>(payload.size())
    );
}

// void MetaWearSdkBridge::feedNotificationPayload(
//     const std::vector<std::uint8_t>& payload
// ) {
//     if (payload.empty()) {
//         std::cerr << "SDK bridge: dropping empty notification payload\n";
//         return;
//     }

//     if (payload.size() < 2) {
//     std::cerr << "SDK bridge: dropping short notification payload ["
//               << payload.size()
//               << " bytes]\n";
//     return;
//     }

//     if (notify_handler_ == nullptr || notify_caller_ == nullptr) {
//         std::cout << "SDK bridge has no notification handler yet; dropping payload\n";
//         return;
//     }

//     if (payload.size() > 255) {
//         throw std::runtime_error("Cannot feed SDK notification payload larger than 255 bytes");
//     }

//     if (payload[0] == 0x0f) {
//     std::cerr
//         << "SDK bridge: dropping unexpected macro-module payload, len="
//         << payload.size()
//         << " payload="
//         << headmotion::util::hexDump(payload)
//         << "\n";

//     return;
//     }

//     notify_handler_(
//         notify_caller_,
//         payload.data(),
//         static_cast<std::uint8_t>(payload.size())
//     );
// }

// void MetaWearSdkBridge::feedNotificationPayload(
//     const std::vector<std::uint8_t>& payload
// ) {
//     if (notify_handler_ == nullptr || notify_caller_ == nullptr) {
//         std::cout << "SDK bridge has no notification handler yet; dropping payload\n";
//         return;
//     }

//     if (payload.size() > 255) {
//         throw std::runtime_error("Cannot feed SDK notification payload larger than 255 bytes");
//     }

//     notify_handler_(
//         notify_caller_,
//         payload.data(),
//         static_cast<std::uint8_t>(payload.size())
//     );
// }
std::vector<std::uint8_t> MetaWearSdkBridge::serializeBoard() const {
    if (board_ == nullptr) {
        throw std::runtime_error("Cannot serialize null MetaWear board");
    }

    uint32_t size = 0;
    uint8_t* raw = mbl_mw_metawearboard_serialize(board_, &size);

    if (raw == nullptr || size == 0) {
        throw std::runtime_error("mbl_mw_metawearboard_serialize returned empty state");
    }

    std::vector<std::uint8_t> out(raw, raw + size);

    /*
     * The SDK allocates this buffer. Some SDK versions expose a dedicated
     * memory-free helper, but this local SDK has varied during development.
     * This CLI process is short-lived, so we intentionally avoid guessing
     * the wrong deallocator here.
     */

    return out;
}

void MetaWearSdkBridge::deserializeBoard(const std::vector<std::uint8_t>& state) {
    if (board_ == nullptr) {
        throw std::runtime_error("Cannot deserialize into null MetaWear board");
    }

    if (state.empty()) {
        throw std::runtime_error("Cannot deserialize empty MetaWear board state");
    }

    mbl_mw_metawearboard_deserialize(
        board_,
        const_cast<std::uint8_t*>(state.data()),
        static_cast<uint32_t>(state.size())
    );
}

void MetaWearSdkBridge::writeGattCharThunk(
    void* context,
    const void* caller,
    MblMwGattCharWriteType write_type,
    const MblMwGattChar* characteristic,
    const std::uint8_t* value,
    std::uint8_t length
) {
    auto* self = static_cast<MetaWearSdkBridge*>(context);
    self->handleWriteGattChar(caller, write_type,characteristic, value, length);
}

void MetaWearSdkBridge::readGattCharThunk(
    void* context,
    const void* caller,
    const MblMwGattChar* characteristic,
    MblMwFnIntVoidPtrArray handler
) {
    auto* self = static_cast<MetaWearSdkBridge*>(context);
    self->handleReadGattChar(caller, characteristic, handler);
}

void MetaWearSdkBridge::enableNotificationsThunk(
    void* context,
    const void* caller,
    const MblMwGattChar* characteristic,
    MblMwFnIntVoidPtrArray handler,
    MblMwFnVoidVoidPtrInt ready
) {
    auto* self = static_cast<MetaWearSdkBridge*>(context);
    self->handleEnableNotifications(caller, characteristic, handler, ready);
}

void MetaWearSdkBridge::onDisconnectThunk(
    void* context,
    const void* caller,
    MblMwFnVoidVoidPtrInt handler
) {
    auto* self = static_cast<MetaWearSdkBridge*>(context);
    self->handleDisconnectSubscribe(caller, handler);
}

void MetaWearSdkBridge::initializedThunk(
    void* context,
    MblMwMetaWearBoard* board,
    int32_t status
) {
    (void)board;

    auto* self = static_cast<MetaWearSdkBridge*>(context);

    self->initialize_status_ = status;
    self->initialized_ = true;

    std::cout << "SDK initialized callback status=" << status << "\n";
}

} // namespace headmotion::sdk
