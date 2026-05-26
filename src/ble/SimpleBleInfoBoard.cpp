#include "headmotion/ble/SimpleBleInfoBoard.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>

namespace headmotion {

namespace {

constexpr int SCAN_MS = 5000;

const SimpleBLE::BluetoothUUID DIS_SERVICE_UUID(
    "0000180a-0000-1000-8000-00805f9b34fb"
);

const SimpleBLE::BluetoothUUID MODEL_NUMBER_UUID(
    "00002a24-0000-1000-8000-00805f9b34fb"
);

const SimpleBLE::BluetoothUUID SERIAL_NUMBER_UUID(
    "00002a25-0000-1000-8000-00805f9b34fb"
);

const SimpleBLE::BluetoothUUID FIRMWARE_REVISION_UUID(
    "00002a26-0000-1000-8000-00805f9b34fb"
);

const SimpleBLE::BluetoothUUID HARDWARE_REVISION_UUID(
    "00002a27-0000-1000-8000-00805f9b34fb"
);

const SimpleBLE::BluetoothUUID MANUFACTURER_NAME_UUID(
    "00002a29-0000-1000-8000-00805f9b34fb"
);

std::string toLower(std::string s) {
    std::transform(
        s.begin(),
        s.end(),
        s.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return s;
}

std::string trimTrailingNulls(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }

    return s;
}

}  // namespace

SimpleBleInfoBoard::~SimpleBleInfoBoard() {
    disconnect();
}

Result SimpleBleInfoBoard::connect(const std::string& target) {
    try {
        if (!SimpleBLE::Adapter::bluetooth_enabled()) {
            return Result::error("Bluetooth is not enabled");
        }

        auto adapters = SimpleBLE::Adapter::get_adapters();
        if (adapters.empty()) {
            return Result::error("No Bluetooth adapters found");
        }

        auto adapter = adapters.at(0);

        adapter.scan_for(SCAN_MS);
        auto peripherals = adapter.scan_get_results();

        for (auto& peripheral : peripherals) {
            if (!targetMatches(peripheral, target)) {
                continue;
            }

            peripheral.connect();

            if (!peripheral.is_connected()) {
                return Result::error("Peripheral connect() returned but device is not connected");
            }

            connectedAddress_ = peripheral.address();
            peripheral_ = peripheral;
            initialized_ = true;

            return Result::success();
        }

        return Result::error("Could not find BLE target: " + target);
    } catch (const std::exception& e) {
        return Result::error(std::string("BLE connect failed: ") + e.what());
    }
}

Result SimpleBleInfoBoard::initialize() {
    if (!peripheral_.has_value()) {
        return Result::error("Cannot initialize before connecting");
    }

    initialized_ = peripheral_->is_connected();

    if (!initialized_) {
        return Result::error("Peripheral is not connected");
    }

    return Result::success();
}

void SimpleBleInfoBoard::disconnect() {
    try {
        if (peripheral_.has_value() && peripheral_->is_connected()) {
            peripheral_->disconnect();
        }
    } catch (...) {
        // Cleanup path should not throw.
    }

    initialized_ = false;
}

bool SimpleBleInfoBoard::isInitialized() const {
    return initialized_;
}

Result SimpleBleInfoBoard::readDeviceInfo(SessionMetadata& metadataOut) {
    if (!peripheral_.has_value()) {
        return Result::error("Cannot read device info before connecting");
    }

    if (!peripheral_->is_connected()) {
        return Result::error("Cannot read device info because peripheral is disconnected");
    }

    try {
        metadataOut.deviceMac = connectedAddress_;

        metadataOut.manufacturer = readStringCharacteristic(
            *peripheral_,
            DIS_SERVICE_UUID,
            MANUFACTURER_NAME_UUID
        );

        metadataOut.modelNumber = readStringCharacteristic(
            *peripheral_,
            DIS_SERVICE_UUID,
            MODEL_NUMBER_UUID
        );

        metadataOut.serialNumber = readStringCharacteristic(
            *peripheral_,
            DIS_SERVICE_UUID,
            SERIAL_NUMBER_UUID
        );

        metadataOut.firmwareRevision = readStringCharacteristic(
            *peripheral_,
            DIS_SERVICE_UUID,
            FIRMWARE_REVISION_UUID
        );

        metadataOut.hardwareRevision = readStringCharacteristic(
            *peripheral_,
            DIS_SERVICE_UUID,
            HARDWARE_REVISION_UUID
        );

        return Result::success();
    } catch (const std::exception& e) {
        return Result::error(std::string("Failed to read device information: ") + e.what());
    }
}

Result SimpleBleInfoBoard::configureHeadMotionSignals(const SessionConfig&) {
    return Result::error("configureHeadMotionSignals is not implemented by SimpleBleInfoBoard");
}

Result SimpleBleInfoBoard::createHeadMotionLoggers(SessionMetadata&) {
    return Result::error("createHeadMotionLoggers is not implemented by SimpleBleInfoBoard");
}

Result SimpleBleInfoBoard::startLogging(bool) {
    return Result::error("startLogging is not implemented by SimpleBleInfoBoard");
}

Result SimpleBleInfoBoard::stopLogging() {
    return Result::error("stopLogging is not implemented by SimpleBleInfoBoard");
}

Result SimpleBleInfoBoard::flushLogPage() {
    return Result::error("flushLogPage is not implemented by SimpleBleInfoBoard");
}

Result SimpleBleInfoBoard::subscribeLoggers(
    const SessionMetadata&,
    std::function<void(const ImuSample&)>
) {
    return Result::error("subscribeLoggers is not implemented by SimpleBleInfoBoard");
}

Result SimpleBleInfoBoard::downloadLogs(
    std::function<void(const DownloadProgress&)>
) {
    return Result::error("downloadLogs is not implemented by SimpleBleInfoBoard");
}

Result SimpleBleInfoBoard::clearLogEntries() {
    return Result::error("clearLogEntries is not implemented by SimpleBleInfoBoard");
}

bool SimpleBleInfoBoard::targetMatches(SimpleBLE::Peripheral& peripheral,
                                       const std::string& target) {
    const std::string targetLower = toLower(target);

    const std::string identifierLower = toLower(peripheral.identifier());
    const std::string addressLower = toLower(peripheral.address());

    return identifierLower == targetLower ||
           addressLower == targetLower ||
           identifierLower.find(targetLower) != std::string::npos;
}

std::string SimpleBleInfoBoard::readStringCharacteristic(
    SimpleBLE::Peripheral& peripheral,
    const SimpleBLE::BluetoothUUID& serviceUuid,
    const SimpleBLE::BluetoothUUID& characteristicUuid
) {
    auto bytes = peripheral.read(serviceUuid, characteristicUuid);

    std::string value;
    value.reserve(bytes.size());

    for (const auto byte : bytes) {
        value.push_back(static_cast<char>(byte));
    }

    return trimTrailingNulls(value);
}

}  // namespace headmotion
