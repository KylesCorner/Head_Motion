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
    const auto frames = usb_.readFrames(std::chrono::milliseconds(timeout_ms));

    for (const auto& frame : frames) {
        std::cout << "SDK bridge RX payload: "
                  << headmotion::util::hexDump(frame.payload)
                  << "\n";

        feedNotificationPayload(frame.payload);
    }
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
    (void)characteristic;

    const std::vector<std::uint8_t> payload(value, value + length);

    std::cout << "SDK write_gatt_char TX payload ["
              << payload.size()
              << " bytes]: "
              << headmotion::util::hexDump(payload)
              << "\n";

    usb_.writePayload(payload);

    /*
     * Some SDK writes produce an immediate module response.
     * Pull a short response window here and feed it back as if it were
     * a BLE notification.
     */
    pumpOnce(150);
}

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
    if (notify_handler_ == nullptr || notify_caller_ == nullptr) {
        std::cout << "SDK bridge has no notification handler yet; dropping payload\n";
        return;
    }

    if (payload.size() > 255) {
        throw std::runtime_error("Cannot feed SDK notification payload larger than 255 bytes");
    }

    notify_handler_(
        notify_caller_,
        payload.data(),
        static_cast<std::uint8_t>(payload.size())
    );
}
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