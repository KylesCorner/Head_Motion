#pragma once

#include "headmotion/metawear/MetaWearUsbTransport.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "metawear/core/metawearboard.h"
#include "metawear/platform/btle_connection.h"
}

namespace headmotion::sdk {

class MetaWearSdkBridge {
public:
    explicit MetaWearSdkBridge(headmotion::metawear::MetaWearUsbTransport& usb);
    ~MetaWearSdkBridge();

    MetaWearSdkBridge(const MetaWearSdkBridge&) = delete;
    MetaWearSdkBridge& operator=(const MetaWearSdkBridge&) = delete;

    MblMwMetaWearBoard* board();
    MblMwBtleConnection* connection();

    bool initialize(int timeout_ms);

    void pumpOnce(int timeout_ms);

    bool initialized() const;
    int initializeStatus() const;

    std::vector<std::uint8_t> serializeBoard() const;
    void deserializeBoard(const std::vector<std::uint8_t>& state);

private:
    headmotion::metawear::MetaWearUsbTransport& usb_;

    MblMwBtleConnection connection_{};
    MblMwMetaWearBoard* board_ = nullptr;

    const void* notify_caller_ = nullptr;
    MblMwFnIntVoidPtrArray notify_handler_ = nullptr;

    std::atomic<bool> initialized_{false};
    std::atomic<int> initialize_status_{-999};

    int dis_read_count_ = 0;

    void handleWriteGattChar(
    const void* caller,
    MblMwGattCharWriteType write_type,
    const MblMwGattChar* characteristic,
    const std::uint8_t* value,
    std::uint8_t length
    );

    void handleReadGattChar(
        const void* caller,
        const MblMwGattChar* characteristic,
        MblMwFnIntVoidPtrArray handler
    );

    void handleEnableNotifications(
        const void* caller,
        const MblMwGattChar* characteristic,
        MblMwFnIntVoidPtrArray handler,
        MblMwFnVoidVoidPtrInt ready
    );

    void handleDisconnectSubscribe(
        const void* caller,
        MblMwFnVoidVoidPtrInt handler
    );

    void feedNotificationPayload(const std::vector<std::uint8_t>& payload);

    static void writeGattCharThunk(
    void* context,
    const void* caller,
    MblMwGattCharWriteType write_type,
    const MblMwGattChar* characteristic,
    const std::uint8_t* value,
    std::uint8_t length
    );

    static void readGattCharThunk(
        void* context,
        const void* caller,
        const MblMwGattChar* characteristic,
        MblMwFnIntVoidPtrArray handler
    );

    static void enableNotificationsThunk(
        void* context,
        const void* caller,
        const MblMwGattChar* characteristic,
        MblMwFnIntVoidPtrArray handler,
        MblMwFnVoidVoidPtrInt ready
    );

    static void onDisconnectThunk(
        void* context,
        const void* caller,
        MblMwFnVoidVoidPtrInt handler
    );

    static void initializedThunk(
        void* context,
        MblMwMetaWearBoard* board,
        int32_t status
    );
};

} // namespace headmotion::sdk