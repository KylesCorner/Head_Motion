/*
 * MMS+ hardware scan integration test.
 *
 * REQUIRES REAL HARDWARE.
 *
 * This test is NON-DESTRUCTIVE:
 *   - does not reset the board
 *   - does not clear logs
 *   - does not start/stop recording
 *
 * It DOES actively open/probe candidate serial ports, so it should
 * not run alongside other hardware tests.
 *
 * Opt-in environment variables:
 *
 *   HEADMOTION_SCAN_EXPECTED_DEVICES
 *       Expected number of physical MMS+ devices.
 *
 *   HEADMOTION_SCAN_EXPECTED_IDS
 *       Optional comma/semicolon-separated list of exact device IDs.
 *
 * At least one of the above must be provided or the test is skipped.
 *
 * Examples:
 *
 *   HEADMOTION_SCAN_EXPECTED_DEVICES=1 \
 *   ./headmotion_test_hardware_scan
 *
 *   HEADMOTION_SCAN_EXPECTED_DEVICES=2 \
 *   HEADMOTION_SCAN_EXPECTED_IDS="0561E1,ABC123" \
 *   ./headmotion_test_hardware_scan
 */

#include "headmotion/app/CommandOutput.hpp"
#include "headmotion/app/Commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>


namespace {

using headmotion::app::CommandEvent;
using headmotion::app::CommandOutput;


constexpr int SKIP_RETURN_CODE = 77;

constexpr std::uint64_t MMS_VENDOR_ID =
    0x1915;

constexpr std::uint64_t MMS_PRODUCT_ID =
    0xd978;


int failures = 0;


/*
 * ============================================================
 * Reporting
 * ============================================================
 */

void fail(
    const std::string& message
) {
    ++failures;

    std::cerr
        << "FAIL: "
        << message
        << '\n';
}


void pass(
    const std::string& message
) {
    std::cout
        << "PASS: "
        << message
        << '\n';
}


/*
 * ============================================================
 * Event capture
 * ============================================================
 */

class EventCapture {
public:
    CommandOutput::Sink sink()
    {
        return [this](
            const CommandEvent& event
        ) {
            std::lock_guard<std::mutex> lock(
                mutex_
            );

            events_.push_back(
                event
            );
        };
    }


    std::vector<CommandEvent> snapshot() const
    {
        std::lock_guard<std::mutex> lock(
            mutex_
        );

        return events_;
    }


private:
    mutable std::mutex mutex_;

    std::vector<CommandEvent>
        events_;
};


/*
 * ============================================================
 * CommandEvent helpers
 * ============================================================
 */

std::optional<std::string> stringField(
    const CommandEvent& event,
    const std::string& key
) {
    for (const auto& field : event.fields) {
        if (field.key != key) {
            continue;
        }

        if (
            const auto* value =
                std::get_if<std::string>(
                    &field.value
                )
        ) {
            return *value;
        }
    }

    return std::nullopt;
}


std::optional<std::uint64_t> uintField(
    const CommandEvent& event,
    const std::string& key
) {
    for (const auto& field : event.fields) {
        if (field.key != key) {
            continue;
        }

        if (
            const auto* value =
                std::get_if<std::uint64_t>(
                    &field.value
                )
        ) {
            return *value;
        }

        if (
            const auto* value =
                std::get_if<std::int64_t>(
                    &field.value
                )
        ) {
            if (*value >= 0) {
                return
                    static_cast<std::uint64_t>(
                        *value
                    );
            }
        }
    }

    return std::nullopt;
}


std::optional<CommandEvent> lastEvent(
    const EventCapture& capture,
    const std::string& type
) {
    const auto events =
        capture.snapshot();

    for (
        auto it = events.rbegin();
        it != events.rend();
        ++it
    ) {
        if (it->type == type) {
            return *it;
        }
    }

    return std::nullopt;
}


/*
 * ============================================================
 * Environment parsing
 * ============================================================
 */

std::string trim(
    std::string value
) {
    while (
        !value.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                value.front()
            )
        )
    ) {
        value.erase(
            value.begin()
        );
    }


    while (
        !value.empty() &&
        std::isspace(
            static_cast<unsigned char>(
                value.back()
            )
        )
    ) {
        value.pop_back();
    }


    return value;
}


std::vector<std::string> parseList(
    const std::string& value
) {
    std::vector<std::string> result;

    std::string current;


    for (const char ch : value) {
        if (
            ch == ',' ||
            ch == ';'
        ) {
            current =
                trim(current);

            if (!current.empty()) {
                result.push_back(
                    current
                );
            }

            current.clear();
            continue;
        }

        current.push_back(ch);
    }


    current =
        trim(current);


    if (!current.empty()) {
        result.push_back(
            current
        );
    }


    return result;
}


std::optional<std::uint64_t>
readExpectedDeviceCount()
{
    const char* value =
        std::getenv(
            "HEADMOTION_SCAN_EXPECTED_DEVICES"
        );


    if (
        value == nullptr ||
        *value == '\0'
    ) {
        return std::nullopt;
    }


    try {
        const std::uint64_t result =
            std::stoull(value);

        if (result == 0) {
            std::cerr
                << "HEADMOTION_SCAN_EXPECTED_DEVICES "
                << "must be greater than zero\n";

            std::exit(2);
        }

        return result;
    }
    catch (...) {
        std::cerr
            << "Invalid HEADMOTION_SCAN_EXPECTED_DEVICES: "
            << value
            << '\n';

        std::exit(2);
    }
}


std::vector<std::string>
readExpectedDeviceIds()
{
    const char* value =
        std::getenv(
            "HEADMOTION_SCAN_EXPECTED_IDS"
        );


    if (
        value == nullptr ||
        *value == '\0'
    ) {
        return {};
    }


    return parseList(
        value
    );
}


/*
 * ============================================================
 * Parsed scan result
 * ============================================================
 */

struct ScanDevice {
    std::string device_id;
    std::string port;
    std::string system_port;
    std::string identity;

    std::uint64_t vendor_id = 0;
    std::uint64_t product_id = 0;


    auto key() const
    {
        return std::tie(
            device_id,
            port,
            system_port,
            identity,
            vendor_id,
            product_id
        );
    }
};


bool operator<(
    const ScanDevice& lhs,
    const ScanDevice& rhs
) {
    return
        lhs.key() <
        rhs.key();
}


bool operator==(
    const ScanDevice& lhs,
    const ScanDevice& rhs
) {
    return
        lhs.key() ==
        rhs.key();
}


struct ScanResult {
    int return_code = -1;

    std::uint64_t serial_ports_found = 0;
    std::uint64_t verified_devices = 0;

    std::vector<ScanDevice> devices;
};


/*
 * ============================================================
 * Execute one real hardware scan
 * ============================================================
 */

ScanResult runHardwareScan(
    std::size_t run_number
) {
    std::cout
        << "\n----------------------------------------\n"
        << "Hardware scan #"
        << run_number
        << "\n"
        << "----------------------------------------\n";


    EventCapture capture;

    CommandOutput output(
        "scan",
        capture.sink()
    );


    ScanResult result;


    try {
        result.return_code =
            headmotion::app::
                runScanPortsCommand(
                    output
                );
    }
    catch (const std::exception& error) {
        fail(
            "scan threw exception: " +
            std::string(
                error.what()
            )
        );

        return result;
    }
    catch (...) {
        fail(
            "scan threw unknown exception"
        );

        return result;
    }


    if (result.return_code != 0) {
        fail(
            "scan returned " +
            std::to_string(
                result.return_code
            )
        );

        return result;
    }


    /*
     * --------------------------------------------------------
     * Parse scan_summary
     * --------------------------------------------------------
     */

    const auto scan_summary =
        lastEvent(
            capture,
            "scan_summary"
        );


    if (!scan_summary) {
        fail(
            "scan emitted no scan_summary event"
        );
    }
    else {
        const auto count =
            uintField(
                *scan_summary,
                "serial_ports_found"
            );


        if (!count) {
            fail(
                "scan_summary missing serial_ports_found"
            );
        }
        else {
            result.serial_ports_found =
                *count;
        }
    }


    /*
     * --------------------------------------------------------
     * Parse verified device events
     * --------------------------------------------------------
     */

    const auto events =
        capture.snapshot();


    for (const auto& event : events) {
        if (event.type != "device") {
            continue;
        }


        ScanDevice device;


        const auto device_id =
            stringField(
                event,
                "device_id"
            );

        const auto port =
            stringField(
                event,
                "port"
            );

        const auto system_port =
            stringField(
                event,
                "system_port"
            );

        const auto identity =
            stringField(
                event,
                "identity"
            );

        const auto vendor_id =
            uintField(
                event,
                "vendor_id"
            );

        const auto product_id =
            uintField(
                event,
                "product_id"
            );


        if (
            !device_id ||
            device_id->empty()
        ) {
            fail(
                "device event contains empty device_id"
            );
        }
        else {
            device.device_id =
                *device_id;
        }


        if (
            !port ||
            port->empty()
        ) {
            fail(
                "device event contains empty preferred port"
            );
        }
        else {
            device.port =
                *port;
        }


        if (
            !system_port ||
            system_port->empty()
        ) {
            fail(
                "device event contains empty system_port"
            );
        }
        else {
            device.system_port =
                *system_port;
        }


        if (
            !identity ||
            identity->empty()
        ) {
            fail(
                "device event contains empty identity"
            );
        }
        else {
            device.identity =
                *identity;
        }


        if (!vendor_id) {
            fail(
                "device event missing vendor_id"
            );
        }
        else {
            device.vendor_id =
                *vendor_id;
        }


        if (!product_id) {
            fail(
                "device event missing product_id"
            );
        }
        else {
            device.product_id =
                *product_id;
        }


        result.devices.push_back(
            std::move(device)
        );
    }


    /*
     * --------------------------------------------------------
     * Parse final summary
     * --------------------------------------------------------
     */

    const auto summary =
        lastEvent(
            capture,
            "summary"
        );


    if (!summary) {
        fail(
            "scan emitted no final summary event"
        );
    }
    else {
        const auto count =
            uintField(
                *summary,
                "verified_devices"
            );


        if (!count) {
            fail(
                "summary missing verified_devices"
            );
        }
        else {
            result.verified_devices =
                *count;
        }
    }


    /*
     * --------------------------------------------------------
     * Internal consistency
     * --------------------------------------------------------
     */

    if (
        result.verified_devices !=
        result.devices.size()
    ) {
        fail(
            "summary reports " +
            std::to_string(
                result.verified_devices
            ) +
            " verified device(s), but " +
            std::to_string(
                result.devices.size()
            ) +
            " device event(s) were emitted"
        );
    }
    else {
        pass(
            "summary device count matches emitted devices"
        );
    }


    if (
        result.serial_ports_found <
        result.devices.size()
    ) {
        fail(
            "verified device count exceeds serial port count"
        );
    }
    else {
        pass(
            "serial port count is consistent with verified devices"
        );
    }


    return result;
}


/*
 * ============================================================
 * Validate one scan
 * ============================================================
 */

void validateScan(
    const ScanResult& scan,
    std::size_t run_number,
    std::uint64_t expected_count,
    const std::set<std::string>& expected_ids
) {
    if (scan.return_code != 0) {
        return;
    }


    /*
     * --------------------------------------------------------
     * Expected count
     * --------------------------------------------------------
     */

    if (
        scan.devices.size() !=
        expected_count
    ) {
        fail(
            "scan #" +
            std::to_string(
                run_number
            ) +
            " expected " +
            std::to_string(
                expected_count
            ) +
            " MMS+ device(s), found " +
            std::to_string(
                scan.devices.size()
            )
        );
    }
    else {
        pass(
            "scan #" +
            std::to_string(
                run_number
            ) +
            " found expected " +
            std::to_string(
                expected_count
            ) +
            " MMS+ device(s)"
        );
    }


    /*
     * --------------------------------------------------------
     * Validate each physical device
     * --------------------------------------------------------
     */

    std::set<std::string>
        discovered_ids;

    std::set<std::string>
        discovered_ports;

    std::set<std::string>
        discovered_system_ports;


    for (const auto& device : scan.devices) {
        std::cout
            << "  MMS+"
            << "\n"
            << "    device_id:   "
            << device.device_id
            << "\n"
            << "    port:        "
            << device.port
            << "\n"
            << "    system_port: "
            << device.system_port
            << "\n"
            << "    identity:    "
            << device.identity
            << "\n"
            << "    VID:PID:     "
            << std::hex
            << device.vendor_id
            << ":"
            << device.product_id
            << std::dec
            << "\n";


        if (
            device.vendor_id !=
            MMS_VENDOR_ID
        ) {
            fail(
                "device " +
                device.device_id +
                " has unexpected vendor ID"
            );
        }


        if (
            device.product_id !=
            MMS_PRODUCT_ID
        ) {
            fail(
                "device " +
                device.device_id +
                " has unexpected product ID"
            );
        }


        if (
            !discovered_ids
                .insert(
                    device.device_id
                )
                .second
        ) {
            fail(
                "duplicate device ID discovered: " +
                device.device_id
            );
        }


        if (
            !discovered_ports
                .insert(
                    device.port
                )
                .second
        ) {
            fail(
                "duplicate preferred port discovered: " +
                device.port
            );
        }


        if (
            !discovered_system_ports
                .insert(
                    device.system_port
                )
                .second
        ) {
            fail(
                "duplicate system port discovered: " +
                device.system_port
            );
        }
    }


    if (
        discovered_ids.size() ==
        scan.devices.size()
    ) {
        pass(
            "all discovered device IDs are unique"
        );
    }


    if (
        discovered_ports.size() ==
        scan.devices.size()
    ) {
        pass(
            "all discovered preferred ports are unique"
        );
    }


    if (
        discovered_system_ports.size() ==
        scan.devices.size()
    ) {
        pass(
            "all discovered system ports are unique"
        );
    }


    /*
     * --------------------------------------------------------
     * Exact expected IDs
     * --------------------------------------------------------
     */

    if (!expected_ids.empty()) {
        if (
            discovered_ids !=
            expected_ids
        ) {
            fail(
                "scan #" +
                std::to_string(
                    run_number
                ) +
                " discovered device IDs do not match "
                "HEADMOTION_SCAN_EXPECTED_IDS"
            );


            std::cerr
                << "  expected:\n";

            for (const auto& id : expected_ids) {
                std::cerr
                    << "    "
                    << id
                    << '\n';
            }


            std::cerr
                << "  discovered:\n";

            for (const auto& id : discovered_ids) {
                std::cerr
                    << "    "
                    << id
                    << '\n';
            }
        }
        else {
            pass(
                "scan #" +
                std::to_string(
                    run_number
                ) +
                " discovered the exact expected device IDs"
            );
        }
    }
}


/*
 * ============================================================
 * Stability across repeated scans
 * ============================================================
 */

void validateRepeatedScanStability(
    const ScanResult& first,
    const ScanResult& second
) {
    if (
        first.return_code != 0 ||
        second.return_code != 0
    ) {
        return;
    }


    std::set<ScanDevice>
        first_devices(
            first.devices.begin(),
            first.devices.end()
        );

    std::set<ScanDevice>
        second_devices(
            second.devices.begin(),
            second.devices.end()
        );


    if (
        first_devices !=
        second_devices
    ) {
        fail(
            "repeated scans returned different device metadata"
        );


        std::cerr
            << "\nFirst scan:\n";

        for (const auto& device : first_devices) {
            std::cerr
                << "  "
                << device.device_id
                << " -> "
                << device.port
                << " -> "
                << device.identity
                << '\n';
        }


        std::cerr
            << "\nSecond scan:\n";

        for (const auto& device : second_devices) {
            std::cerr
                << "  "
                << device.device_id
                << " -> "
                << device.port
                << " -> "
                << device.identity
                << '\n';
        }
    }
    else {
        pass(
            "repeated scans returned identical MMS+ metadata"
        );
    }


    if (
        first.verified_devices !=
        second.verified_devices
    ) {
        fail(
            "verified device count changed between scans"
        );
    }
    else {
        pass(
            "verified device count stable across repeated scans"
        );
    }
}

} // namespace


int main()
{
    const auto configured_count =
        readExpectedDeviceCount();

    const std::vector<std::string>
        expected_id_list =
            readExpectedDeviceIds();


    /*
     * This is an opt-in real-hardware test.
     */
    if (
        !configured_count &&
        expected_id_list.empty()
    ) {
        std::cout
            << "SKIP: hardware scan integration test is not configured\n"
            << "\n"
            << "Set one of:\n"
            << "  HEADMOTION_SCAN_EXPECTED_DEVICES\n"
            << "  HEADMOTION_SCAN_EXPECTED_IDS\n";

        return SKIP_RETURN_CODE;
    }


    const std::set<std::string>
        expected_ids(
            expected_id_list.begin(),
            expected_id_list.end()
        );


    if (
        expected_ids.size() !=
        expected_id_list.size()
    ) {
        std::cerr
            << "ERROR: HEADMOTION_SCAN_EXPECTED_IDS contains "
            << "duplicate device IDs\n";

        return 2;
    }


    /*
     * If exact IDs were supplied without a count, infer the expected
     * count from the ID list.
     */
    const std::uint64_t expected_count =
        configured_count
            ? *configured_count
            : static_cast<std::uint64_t>(
                expected_ids.size()
            );


    /*
     * If both were supplied, make sure configuration itself is
     * internally consistent.
     */
    if (
        !expected_ids.empty() &&
        expected_ids.size() !=
            expected_count
    ) {
        std::cerr
            << "ERROR: HEADMOTION_SCAN_EXPECTED_DEVICES="
            << expected_count
            << " but HEADMOTION_SCAN_EXPECTED_IDS contains "
            << expected_ids.size()
            << " ID(s)\n";

        return 2;
    }


    std::cout
        << "========================================\n"
        << "HeadMotion Hardware Scan Integration Test\n"
        << "========================================\n\n"
        << "Expected MMS+ devices: "
        << expected_count
        << '\n';


    if (!expected_ids.empty()) {
        std::cout
            << "Expected device IDs:\n";

        for (const auto& id : expected_ids) {
            std::cout
                << "  "
                << id
                << '\n';
        }
    }


    /*
     * Run twice to verify that discovery/device identity is stable
     * across independent scans.
     */
    const ScanResult first =
        runHardwareScan(
            1
        );


    validateScan(
        first,
        1,
        expected_count,
        expected_ids
    );


    const ScanResult second =
        runHardwareScan(
            2
        );


    validateScan(
        second,
        2,
        expected_count,
        expected_ids
    );


    validateRepeatedScanStability(
        first,
        second
    );


    std::cout
        << "\n========================================\n"
        << "Hardware Scan Result\n"
        << "========================================\n";


    if (failures != 0) {
        std::cerr
            << failures
            << " hardware scan assertion(s) FAILED\n";

        return 1;
    }


    std::cout
        << "PASS: real serial discovery completed\n"
        << "PASS: expected MMS+ count found\n"
        << "PASS: protocol verification succeeded\n"
        << "PASS: device IDs are unique\n"
        << "PASS: device ports are unique\n"
        << "PASS: VID/PID values are correct\n"
        << "PASS: repeated discovery is stable\n"
        << "\n"
        << "HARDWARE SCAN INTEGRATION TEST PASSED\n";


    return 0;
}