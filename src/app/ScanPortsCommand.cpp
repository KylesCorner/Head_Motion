#include "headmotion/app/MmsDeviceProbe.hpp"
#include "headmotion/session/DevicePortStore.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <iomanip>
#include <iostream>
#include <vector>
namespace {

constexpr std::uint16_t MMS_VENDOR_ID = 0x1915;
constexpr std::uint16_t MMS_PRODUCT_ID = 0xd978;

bool exactMmsUsbMatch(
    const headmotion::transport::SerialPortInfo& port
) {
    return port.vendor_id == MMS_VENDOR_ID &&
           port.product_id == MMS_PRODUCT_ID;
}

struct VerifiedMms {
    headmotion::transport::SerialPortInfo port;
    std::string identity;
};

} // namespace

namespace headmotion::app {

int runScanPortsCommand() {
    const auto ports =
        headmotion::transport::SerialPortFactory::listPorts();

    if (ports.empty()) {
        std::cerr << "No serial ports found.\n";
        return 2;
    }

    std::vector<VerifiedMms> verified_devices;

    std::cout << "Serial ports:\n";

    for (const auto& port : ports) {
        std::cout << "  " << port.path;

        if (!port.symlink_path.empty()) {
            std::cout << " via " << port.symlink_path;
        }

        if (port.vendor_id != 0 || port.product_id != 0) {
            std::cout
                << " [USB "
                << std::hex
                << std::setw(4)
                << std::setfill('0')
                << port.vendor_id
                << ":"
                << std::setw(4)
                << port.product_id
                << std::dec
                << "]";
        }

        if (!port.serial_number.empty()) {
            std::cout << " serial=" << port.serial_number;
        }

        const bool candidate =
            exactMmsUsbMatch(port) || port.likely_mms;

        if (!candidate) {
            std::cout << "\n";
            continue;
        }

        std::cout << " MMS candidate";

        const auto probe =
            probeMmsDevice(port.preferredPath());

        if (!probe) {
            std::cout << " - protocol verification failed\n";
            continue;
        }

        std::cout
            << " - verified: "
            << probe->identity
            << "\n";

        verified_devices.push_back({
            .port = port,
            .identity = probe->identity
        });
    }

    if (verified_devices.empty()) {
        std::cerr
            << "No verified MMS+ device found. "
            << "The saved default port was not changed.\n";

        return 3;
    }

    if (verified_devices.size() > 1) {
        std::cerr
            << "Multiple verified MMS+ devices found. "
            << "The saved default port was not changed.\n";

        for (const auto& device : verified_devices) {
            std::cerr
                << "  "
                << device.port.preferredPath()
                << "\n";
        }

        return 4;
    }

    const auto& selected = verified_devices.front().port;

    headmotion::session::SavedDevicePort saved;
    saved.preferred_path = selected.preferredPath();
    saved.system_path = selected.path;
    saved.vendor_id = selected.vendor_id;
    saved.product_id = selected.product_id;
    saved.serial_number = selected.serial_number;

    const auto store_path =
        headmotion::session::DevicePortStore::defaultPath();

    headmotion::session::DevicePortStore::save(
        store_path,
        saved
    );

    std::cout
        << "Saved default MMS+ port: "
        << saved.preferred_path
        << "\n"
        << "Port record: "
        << store_path
        << "\n";

    return 0;
}
} // namespace headmotion::app
