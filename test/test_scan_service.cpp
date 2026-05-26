#include "headmotion/app/ScanService.h"

#include "FakeBleAdapter.h"
#include "TestLogger.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace headmotion;
using namespace headmotion::test;

#define CHECK_TRUE(expr)                                                        \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__              \
                      << " expected true: " #expr "\n";                       \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define CHECK_FALSE(expr)                                                       \
    do {                                                                        \
        if ((expr)) {                                                           \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__              \
                      << " expected false: " #expr "\n";                      \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        const auto& av = (a);                                                    \
        const auto& bv = (b);                                                    \
        if (!(av == bv)) {                                                       \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__              \
                      << " expected " #a " == " #b "\n";                     \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

static void test_scan_calls_ble_adapter() {
    FakeBleAdapter ble;
    TestLogger logger;

    ScanService service(ble, logger);

    std::vector<BleDeviceInfo> devices;

    Result result = service.run(devices);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(ble.scanCalled);
}

static void test_scan_returns_devices_from_ble_adapter() {
    FakeBleAdapter ble;
    TestLogger logger;

    BleDeviceInfo metawear;
    metawear.identifier = "MetaWear";
    metawear.address = "F9:CB:94:04:C3:45";
    metawear.rssi = -55;

    BleDeviceInfo other;
    other.identifier = "OtherDevice";
    other.address = "AA:BB:CC:DD:EE:FF";
    other.rssi = -80;

    ble.devicesToReturn.push_back(metawear);
    ble.devicesToReturn.push_back(other);

    ScanService service(ble, logger);

    std::vector<BleDeviceInfo> devices;

    Result result = service.run(devices);

    CHECK_TRUE(result.ok());
    CHECK_EQ(devices.size(), static_cast<size_t>(2));

    CHECK_EQ(devices.at(0).identifier, std::string("MetaWear"));
    CHECK_EQ(devices.at(0).address, std::string("F9:CB:94:04:C3:45"));
    CHECK_EQ(devices.at(0).rssi, static_cast<int16_t>(-55));

    CHECK_EQ(devices.at(1).identifier, std::string("OtherDevice"));
    CHECK_EQ(devices.at(1).address, std::string("AA:BB:CC:DD:EE:FF"));
    CHECK_EQ(devices.at(1).rssi, static_cast<int16_t>(-80));
}

static void test_scan_clears_existing_output_before_running() {
    FakeBleAdapter ble;
    TestLogger logger;

    BleDeviceInfo metawear;
    metawear.identifier = "MetaWear";
    metawear.address = "F9:CB:94:04:C3:45";
    metawear.rssi = -55;

    ble.devicesToReturn.push_back(metawear);

    ScanService service(ble, logger);

    std::vector<BleDeviceInfo> devices;

    BleDeviceInfo stale;
    stale.identifier = "StaleDevice";
    stale.address = "00:00:00:00:00:00";
    stale.rssi = -100;

    devices.push_back(stale);

    Result result = service.run(devices);

    CHECK_TRUE(result.ok());
    CHECK_EQ(devices.size(), static_cast<size_t>(1));
    CHECK_EQ(devices.at(0).identifier, std::string("MetaWear"));
}

static void test_scan_returns_error_if_ble_adapter_fails() {
    FakeBleAdapter ble;
    TestLogger logger;

    ble.scanResult = Result::error("Bluetooth is not enabled");

    ScanService service(ble, logger);

    std::vector<BleDeviceInfo> devices;

    Result result = service.run(devices);

    CHECK_FALSE(result.ok());
    CHECK_TRUE(ble.scanCalled);
    CHECK_TRUE(devices.empty());
    CHECK_EQ(logger.errorMessages.size(), static_cast<size_t>(1));
}

static void test_scan_allows_empty_scan_results() {
    FakeBleAdapter ble;
    TestLogger logger;

    ScanService service(ble, logger);

    std::vector<BleDeviceInfo> devices;

    Result result = service.run(devices);

    CHECK_TRUE(result.ok());
    CHECK_TRUE(ble.scanCalled);
    CHECK_TRUE(devices.empty());
    CHECK_EQ(logger.infoMessages.size(), static_cast<size_t>(1));
}

int main() {
    test_scan_calls_ble_adapter();
    test_scan_returns_devices_from_ble_adapter();
    test_scan_clears_existing_output_before_running();
    test_scan_returns_error_if_ble_adapter_fails();
    test_scan_allows_empty_scan_results();

    std::cout << "[PASS] ScanService tests\n";
    return 0;
}
