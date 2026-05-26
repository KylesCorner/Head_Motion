# HeadMotion Project Context

## Project Goal

HeadMotion is a C++ project for recording human head motion over long periods of time using the MbientLab MetaMotionS+ / MMS+ sensor.

The intended production workflow is:

1. The laptop connects to the MMS+ over BLE.
2. The laptop configures the MMS+ to log IMU data locally on the sensor.
3. The user wears the sensor for up to ~8 hours per day.
4. The MMS+ stores IMU data on its internal NAND flash while disconnected from the laptop.
5. When the user comes back near the computer, the laptop reconnects.
6. The laptop stops logging, flushes final storage pages, downloads all logged data, writes it to disk, verifies the output, and only then clears the sensor log memory.
7. Optionally, the laptop restarts a new logging session.

The primary sensor for now is:

- MbientLab MetaMotionS+ / MMS+
- Store page: https://mbientlab.com/store/metamotions-p/
- Device seen over BLE as: `MetaWear`
- Device MAC used during testing: `F9:CB:94:04:C3:45`
- Serial number from probe: `0561E1`
- Firmware revision from probe: `1.7.2`
- Hardware revision from probe: `0.1`
- Model number from probe: `8`
- Manufacturer: `MbientLab Inc`

There was also discussion of an Xsens `XS-T01`, but that is deferred for now. Focus is currently only on the MbientLab MetaMotion sensor.

---

## Development Environment

Current development machine:

- Arch Linux laptop
- C++ project
- Bluetooth adapter present and working
- BLE stack: BlueZ
- BLE C++ library: SimpleBLE from AUR
- MbientLab SDK: MetaWear C++ SDK built locally
- Build system for current probe: CMake with a Makefile wrapper

Important packages installed / needed:

```bash
sudo pacman -S --needed base-devel git clang cmake make bluez bluez-utils dbus pkgconf
```

SimpleBLE was installed from AUR:

```bash
yay -S simpleble
```

or equivalent:

```bash
paru -S simpleble
```

Bluetooth service:

```bash
sudo systemctl enable --now bluetooth
```

---

## MbientLab C++ SDK Notes

The MbientLab C++ SDK is not a full desktop app and does not provide BLE transport by itself.

It provides the MetaWear protocol layer and expects the application to implement BLE callbacks using a platform BLE library.

The SDK repository used:

```bash
git clone https://github.com/mbientlab/MetaWear-SDK-Cpp.git
```

The SDK was built with:

```bash
make CXX=clang++
```

The SDK initially failed to build on modern Arch/Clang because the bundled `nlohmann::json` header used deprecated literal operator syntax and warnings were treated as errors.

Error looked like:

```text
error: identifier '_json' preceded by whitespace in a literal operator declaration is deprecated [-Werror,-Wdeprecated-literal-operator]
inline nlohmann::json operator "" _json(const char* s, std::size_t)
```

Patch applied:

```bash
perl -pi -e 's/operator "" _json/operator""_json/g' src/metawear/dfu/cpp/json.hpp
perl -pi -e 's/operator "" _json_pointer/operator""_json_pointer/g' src/metawear/dfu/cpp/json.hpp
```

Then:

```bash
make clean
make CXX=clang++
```

The built library should exist at something like:

```text
MetaWear-SDK-Cpp/dist/release/lib/x64/libmetawear.so
```

The SDK smoke test passed:

```text
HeadMotion MetaWear SDK smoke test
Created MetaWear board object successfully
Freed MetaWear board object successfully
```

This proved that:

- SDK headers compile
- `libmetawear.so` links
- runtime loader can find the library
- basic board object allocation works

---

## Current Working Probe

A native Linux C++ probe was created to test the BLE connection layer.

It uses:

```text
HeadMotion C++ probe
  ↓
SimpleBLE
  ↓
BlueZ / D-Bus
  ↓
MMS+
  ↓
MbientLab C++ SDK callbacks
```

The probe implements the required `MblMwBtleConnection` callbacks:

```cpp
MblMwBtleConnection connection = {};
connection.context = &ctx;
connection.write_gatt_char = write_gatt_char_cb;
connection.read_gatt_char = read_gatt_char_cb;
connection.enable_notifications = enable_notifications_cb;
connection.on_disconnect = on_disconnect_cb;
```

The callbacks are backed by SimpleBLE:

- `write_gatt_char_cb` calls `peripheral.write_request(...)` or `peripheral.write_command(...)`
- `read_gatt_char_cb` calls `peripheral.read(...)`
- `enable_notifications_cb` calls `peripheral.notify(...)`
- `on_disconnect_cb` registers SimpleBLE disconnect callback

The MetaWear service/characteristic UUIDs seen in the logs:

```text
MetaWear service:
326a9000-85cb-9195-d9dd-464cfbbae75a

Command characteristic:
326a9001-85cb-9195-d9dd-464cfbbae75a

Notify characteristic:
326a9006-85cb-9195-d9dd-464cfbbae75a
```

Device information service reads worked:

```text
Firmware revision characteristic 00002a26:
1.7.2

Model number characteristic 00002a24:
8

Hardware revision characteristic 00002a27:
0.1

Manufacturer characteristic 00002a29:
MbientLab Inc

Serial number characteristic 00002a25:
0561E1
```

The probe initially timed out during initialization:

```text
[MetaWear initialized] status=16
```

This was fixed by increasing the MetaWear SDK response timeout after board creation:

```cpp
mbl_mw_metawearboard_set_time_for_response(board, 2000);
```

After this, initialization succeeded:

```text
[MetaWear initialized] status=0
Device information:
  manufacturer: MbientLab Inc
  model number: 8
  serial number: 0561E1
  firmware rev:  1.7.2
  hardware rev:  0.1
mbl_mw_metawearboard_is_initialized=1
```

This proves:

```text
BLE scan works
BLE connect works
MetaWear service UUID is correct
notification characteristic is correct
command characteristic is correct
Device Information reads work
MetaWear command/response packets work
MetaWear board initialization works
```

---

## Current Build Setup

The current project folder during probe work was something like:

```text
/home/kyle/Collage/HeadMotion/code/smoke_test
```

The CMake file needed explicit D-Bus linkage because SimpleBLE from AUR did not automatically pull in `dbus-1`.

Working `CMakeLists.txt` shape:

```cmake
cmake_minimum_required(VERSION 3.20)
project(headmotion_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(METAWEAR_SDK_DIR "$ENV{HOME}/Collage/HeadMotion/code/MetaWear-SDK-Cpp")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

add_executable(headmotion_probe
    src/main.cpp
)

target_include_directories(headmotion_probe PRIVATE
    ${METAWEAR_SDK_DIR}/src
    ${DBUS_INCLUDE_DIRS}
)

target_link_directories(headmotion_probe PRIVATE
    ${METAWEAR_SDK_DIR}/dist/release/lib/x64
    ${DBUS_LIBRARY_DIRS}
)

target_link_libraries(headmotion_probe PRIVATE
    metawear
    simpleble
    ${DBUS_LIBRARIES}
)

target_compile_options(headmotion_probe PRIVATE
    ${DBUS_CFLAGS_OTHER}
)

target_link_options(headmotion_probe PRIVATE
    "-Wl,-rpath,${METAWEAR_SDK_DIR}/dist/release/lib/x64"
)
```

Makefile wrapper:

```makefile
BUILD_DIR := build
BUILD_TYPE ?= Debug
TARGET := headmotion_probe

.PHONY: all configure build run clean rebuild deps check-dbus

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j"$$(nproc)"

run: build
	./$(BUILD_DIR)/$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build

deps:
	sudo pacman -S --needed dbus pkgconf bluez bluez-utils

check-dbus:
	pkg-config --cflags --libs dbus-1
```

Important Makefile gotcha encountered:

The file must be named:

```text
Makefile
```

not:

```text
MakeFile
```

GNU `make` looks for:

```text
GNUmakefile
makefile
Makefile
```

---

## Logging / Storage Concept

The MMS+ storage should be treated as **MetaWear log memory**, not a filesystem.

You do not mount it like a USB drive.

The normal flow is:

```text
1. Initialize board
2. Configure sensor signal
3. Create logger for that signal
4. Start logging
5. Disconnect if desired
6. Later reconnect
7. Stop logging
8. Flush page
9. Subscribe to logger callbacks
10. Download logs
11. Write samples to local files
12. Verify files
13. Clear log entries only after successful download
```

For MMS / MetaMotionS NAND flash, an important step before download is:

```cpp
mbl_mw_logging_flush_page(board);
```

This flushes the final partial NAND page before a full download.

Do not clear device logs until:

```text
1. download reports entries_left == 0
2. CSV/SQLite file is flushed and closed
3. row count is nonzero / expected
4. metadata file is written
5. output is verified
```

Then and only then call:

```cpp
mbl_mw_logging_clear_entries(board);
```

---

## Intended Head Motion Logging Profile

For first production attempt, use raw IMU:

```text
Primary:
- Accelerometer x/y/z
- Gyroscope x/y/z

Recommended starting rate:
- Accelerometer: 50 Hz
- Gyroscope: 50 Hz

Optional later:
- Quaternion from sensor fusion
- Euler angles
- Calibration status
- Battery level
```

Reasoning:

```text
- Raw accel + gyro is scientifically defensible.
- Quaternion is convenient for orientation but depends on Bosch fusion behavior.
- Euler angles are convenient but can wrap and have gimbal issues.
- Quaternion is preferred over Euler for orientation if fusion is used.
```

Storage estimate:

```text
8 hours = 28,800 seconds

accel @ 50 Hz = 1,440,000 entries
gyro  @ 50 Hz = 1,440,000 entries
total = 2,880,000 entries
```

This should be feasible on MMS+ internal storage.

Even accel + gyro at 100 Hz:

```text
100 Hz * 28,800 sec * 2 signals = 5,760,000 entries
```

The bigger practical constraints are:

```text
- battery life
- BLE download time
- reliability of reconnect/download
- preventing accidental log erasure
```

---

## Target CLI Design

The eventual tool could be:

```bash
headmotion_mms scan
headmotion_mms info F9:CB:94:04:C3:45
headmotion_mms record-start F9:CB:94:04:C3:45 --acc-hz 50 --gyro-hz 50 --erase
headmotion_mms sync F9:CB:94:04:C3:45 --out ~/HeadMotion/data
headmotion_mms record-stop F9:CB:94:04:C3:45
headmotion_mms daemon --device F9:CB:94:04:C3:45 --out ~/HeadMotion/data
```

First useful version should implement only:

```bash
headmotion_mms record-start MetaWear
headmotion_mms sync MetaWear
```

---

## Desired Application Architecture

The C++ code should be modular and highly unit testable.

The architecture should separate:

```text
main()
  ↓
CLI / command dispatch
  ↓
Application services: record-start, sync, info
  ↓
Domain logic: sessions, logger metadata, download state, sample routing
  ↓
Interfaces: BLE, MetaWear board, clock, filesystem, CSV writer
  ↓
Adapters: SimpleBLE, MbientLab C SDK, real filesystem
```

Main rule:

```text
Keep MbientLab C SDK types out of application/service/domain layers.
```

Good service-level API:

```cpp
Result createHeadMotionLoggers(SessionMetadata& metadata);
```

Bad service-level code:

```cpp
MblMwDataSignal* acc_signal = mbl_mw_acc_get_acceleration_data_signal(board);
mbl_mw_datasignal_log(acc_signal, ...);
```

That direct SDK code should live only in:

```text
src/metawear/MetaWearBoardAdapter.cpp
```

---

## Proposed Project Layout

```text
HeadMotion/
├── CMakeLists.txt
├── Makefile
├── include/
│   └── headmotion/
│       ├── app/
│       │   ├── HeadMotionApp.h
│       │   ├── RecordStartService.h
│       │   ├── SyncService.h
│       │   └── InfoService.h
│       │
│       ├── core/
│       │   ├── SessionConfig.h
│       │   ├── SessionMetadata.h
│       │   ├── HeadMotionSample.h
│       │   ├── DownloadProgress.h
│       │   └── Result.h
│       │
│       ├── interfaces/
│       │   ├── IBleAdapter.h
│       │   ├── IMetaWearBoard.h
│       │   ├── IHeadMotionRecorder.h
│       │   ├── IHeadMotionDownloader.h
│       │   ├── ISampleSink.h
│       │   ├── IMetadataStore.h
│       │   ├── IClock.h
│       │   └── ILogger.h
│       │
│       ├── metawear/
│       │   ├── MetaWearBoardAdapter.h
│       │   ├── MetaWearConnection.h
│       │   ├── MetaWearRecorder.h
│       │   └── MetaWearDownloader.h
│       │
│       ├── ble/
│       │   └── SimpleBleAdapter.h
│       │
│       ├── storage/
│       │   ├── CsvSampleSink.h
│       │   ├── JsonMetadataStore.h
│       │   └── SessionPaths.h
│       │
│       └── util/
│           ├── Timeout.h
│           └── ByteUtils.h
│
├── src/
│   ├── main.cpp
│   ├── app/
│   ├── metawear/
│   ├── ble/
│   └── storage/
│
├── test/
│   ├── CMakeLists.txt
│   ├── support/
│   │   ├── FakeBleAdapter.h
│   │   ├── FakeMetaWearBoard.h
│   │   ├── FakeSampleSink.h
│   │   ├── FakeMetadataStore.h
│   │   ├── FakeClock.h
│   │   └── TestLogger.h
│   │
│   ├── test_record_start_service.cpp
│   ├── test_sync_service.cpp
│   ├── test_session_metadata.cpp
│   └── test_sample_routing.cpp
│
└── data/
```

---

## Core Types

### `SessionConfig`

```cpp
#pragma once

struct SessionConfig {
    float accelHz = 50.0f;
    float gyroHz = 50.0f;
    float accelRangeG = 16.0f;
    float gyroRangeDps = 2000.0f;
    bool eraseBeforeStart = false;
};
```

### `SessionMetadata`

```cpp
#pragma once

#include <cstdint>
#include <string>

struct SessionMetadata {
    std::string sessionId;
    std::string deviceMac;
    std::string serialNumber;
    std::string firmwareRevision;

    uint8_t accelLoggerId = 0xff;
    uint8_t gyroLoggerId = 0xff;

    float accelHz = 50.0f;
    float gyroHz = 50.0f;

    int64_t startedEpochMs = 0;
};
```

### `ImuSample`

```cpp
#pragma once

#include <cstdint>

struct ImuSample {
    enum class Kind {
        Accel,
        Gyro,
        Quaternion
    };

    Kind kind;
    int64_t epochMs;

    float x;
    float y;
    float z;
    float w = 0.0f;
};
```

### `DownloadProgress`

```cpp
#pragma once

#include <cstdint>

struct DownloadProgress {
    uint32_t entriesLeft = 0;
    uint32_t totalEntries = 0;
};
```

### `Result`

Use a small result type instead of throwing exceptions through application logic.

Example concept:

```cpp
#pragma once

#include <string>
#include <utility>

class Result {
public:
    static Result ok() {
        return Result(true, "");
    }

    static Result error(std::string message) {
        return Result(false, std::move(message));
    }

    bool ok() const {
        return ok_;
    }

    const std::string& message() const {
        return message_;
    }

private:
    Result(bool ok, std::string message)
        : ok_(ok), message_(std::move(message)) {}

    bool ok_;
    std::string message_;
};
```

---

## Key Interfaces

### `IMetaWearBoard`

This is the most important abstraction. App services should talk to this, not directly to the C SDK.

```cpp
#pragma once

#include "headmotion/core/SessionConfig.h"
#include "headmotion/core/SessionMetadata.h"
#include "headmotion/core/HeadMotionSample.h"
#include "headmotion/core/DownloadProgress.h"
#include "headmotion/core/Result.h"

#include <functional>
#include <string>

class IMetaWearBoard {
public:
    virtual ~IMetaWearBoard() = default;

    virtual Result connect(const std::string& target) = 0;
    virtual Result initialize() = 0;
    virtual void disconnect() = 0;

    virtual bool isInitialized() const = 0;

    virtual Result readDeviceInfo(SessionMetadata& metadataOut) = 0;

    virtual Result configureHeadMotionSignals(const SessionConfig& config) = 0;

    virtual Result createHeadMotionLoggers(SessionMetadata& metadataInOut) = 0;

    virtual Result startLogging(bool overwrite) = 0;
    virtual Result stopLogging() = 0;

    virtual Result flushLogPage() = 0;

    virtual Result subscribeLoggers(
        const SessionMetadata& metadata,
        std::function<void(const ImuSample&)> onSample
    ) = 0;

    virtual Result downloadLogs(
        std::function<void(const DownloadProgress&)> onProgress
    ) = 0;

    virtual Result clearLogEntries() = 0;
};
```

Real implementation:

```cpp
class MetaWearBoardAdapter : public IMetaWearBoard {
    // wraps MblMwMetaWearBoard*, MblMwBtleConnection, callbacks, etc.
};
```

Fake implementation for tests:

```cpp
class FakeMetaWearBoard : public IMetaWearBoard {
    // records calls, simulates success/failure, emits fake samples
};
```

---

### `ISampleSink`

Separates download logic from CSV/SQLite/Parquet output.

```cpp
#pragma once

#include "headmotion/core/HeadMotionSample.h"
#include "headmotion/core/Result.h"

#include <cstddef>
#include <string>

class ISampleSink {
public:
    virtual ~ISampleSink() = default;

    virtual Result openSession(const std::string& sessionId) = 0;
    virtual Result writeSample(const ImuSample& sample) = 0;
    virtual Result closeSession() = 0;

    virtual size_t sampleCount() const = 0;
};
```

Real implementation:

```cpp
class CsvSampleSink : public ISampleSink {
public:
    Result openSession(const std::string& sessionId) override;
    Result writeSample(const ImuSample& sample) override;
    Result closeSession() override;
    size_t sampleCount() const override;

private:
    std::ofstream accelCsv_;
    std::ofstream gyroCsv_;
    size_t count_ = 0;
};
```

Fake implementation:

```cpp
class FakeSampleSink : public ISampleSink {
public:
    std::vector<ImuSample> samples;
};
```

---

### `IMetadataStore`

Keeps logger IDs and session metadata separate from logging/download code.

```cpp
#pragma once

#include "headmotion/core/SessionMetadata.h"
#include "headmotion/core/Result.h"

#include <optional>
#include <string>

class IMetadataStore {
public:
    virtual ~IMetadataStore() = default;

    virtual Result save(const SessionMetadata& metadata) = 0;

    virtual std::optional<SessionMetadata>
    loadLatestForDevice(const std::string& deviceMacOrSerial) = 0;

    virtual Result markDownloaded(const std::string& sessionId) = 0;
};
```

Real implementation:

```text
JsonMetadataStore
```

Fake implementation:

```text
FakeMetadataStore
```

---

## Application Services

### `RecordStartService`

Responsibilities:

```text
- Connect to device
- Initialize board
- Optionally clear old logs
- Configure accel/gyro
- Create loggers
- Save session metadata
- Start logging
- Disconnect
```

Example shape:

```cpp
class RecordStartService {
public:
    RecordStartService(IMetaWearBoard& board,
                       IMetadataStore& metadataStore,
                       ILogger& logger)
        : board_(board),
          metadataStore_(metadataStore),
          logger_(logger) {}

    Result run(const std::string& target, const SessionConfig& config) {
        auto r = board_.connect(target);
        if (!r.ok()) return r;

        r = board_.initialize();
        if (!r.ok()) return r;

        if (!board_.isInitialized()) {
            return Result::error("Board failed to initialize");
        }

        SessionMetadata metadata;
        r = board_.readDeviceInfo(metadata);
        if (!r.ok()) return r;

        metadata.sessionId = makeSessionId(metadata.serialNumber);
        metadata.accelHz = config.accelHz;
        metadata.gyroHz = config.gyroHz;

        if (config.eraseBeforeStart) {
            r = board_.clearLogEntries();
            if (!r.ok()) return r;
        }

        r = board_.configureHeadMotionSignals(config);
        if (!r.ok()) return r;

        r = board_.createHeadMotionLoggers(metadata);
        if (!r.ok()) return r;

        r = metadataStore_.save(metadata);
        if (!r.ok()) return r;

        r = board_.startLogging(/*overwrite=*/true);
        if (!r.ok()) return r;

        board_.disconnect();

        logger_.info("HeadMotion logging started: " + metadata.sessionId);
        return Result::ok();
    }

private:
    IMetaWearBoard& board_;
    IMetadataStore& metadataStore_;
    ILogger& logger_;
};
```

Important behavior:

```text
- If connect fails, nothing else runs.
- If initialize fails, no loggers are created.
- If eraseBeforeStart is true, clearLogEntries is called.
- Metadata is saved before logging starts.
- Logging starts only after logger creation succeeds.
- Disconnect happens at the end.
```

---

### `SyncService`

Responsibilities:

```text
- Connect to device
- Initialize board
- Load session metadata
- Stop logging
- Flush MMS NAND page
- Open output files
- Subscribe to logger callbacks
- Download logs
- Route samples to output
- Verify output
- Mark session downloaded
- Clear device logs only after verified success
```

Suggested sync state machine:

```cpp
enum class SyncState {
    Idle,
    Connecting,
    Initializing,
    LoadingMetadata,
    StoppingLogging,
    FlushingPage,
    OpeningOutput,
    SubscribingLoggers,
    Downloading,
    VerifyingOutput,
    MarkingDownloaded,
    ClearingDeviceLogs,
    Complete,
    Failed
};
```

This is important because BLE download can fail. Data loss prevention should be explicit.

---

## Suggested Unit Tests

### Record-start tests

```text
test_record_start_connects_and_initializes_board
test_record_start_configures_accel_and_gyro
test_record_start_creates_loggers_and_saves_metadata
test_record_start_starts_logging_after_metadata_save
test_record_start_erases_existing_logs_when_requested
test_record_start_does_not_start_logging_if_logger_creation_fails
test_record_start_disconnects_on_success
```

### Sync tests

```text
test_sync_loads_latest_metadata
test_sync_stops_logging_before_download
test_sync_flushes_mms_page_before_download
test_sync_subscribes_loggers_before_download
test_sync_routes_accel_samples_to_sink
test_sync_routes_gyro_samples_to_sink
test_sync_marks_session_downloaded_after_success
test_sync_does_not_clear_logs_if_download_times_out
test_sync_does_not_clear_logs_if_csv_close_fails
test_sync_clears_device_logs_after_verified_download
```

### Metadata tests

```text
test_metadata_round_trip_json
test_metadata_rejects_missing_logger_ids
test_metadata_uses_serial_number_in_session_path
test_metadata_marks_downloaded_without_destroying_original_config
```

### Sample formatting tests

```text
test_accel_sample_csv_line
test_gyro_sample_csv_line
test_quaternion_sample_csv_line
test_epoch_timestamp_preserved
```

---

## Example Fake Board for Tests

```cpp
class FakeMetaWearBoard : public IMetaWearBoard {
public:
    bool connectCalled = false;
    bool initializeCalled = false;
    bool clearCalled = false;
    bool configureCalled = false;
    bool createLoggersCalled = false;
    bool startLoggingCalled = false;
    bool disconnectCalled = false;

    Result connectResult = Result::ok();
    Result initializeResult = Result::ok();

    Result connect(const std::string&) override {
        connectCalled = true;
        return connectResult;
    }

    Result initialize() override {
        initializeCalled = true;
        return initializeResult;
    }

    void disconnect() override {
        disconnectCalled = true;
    }

    bool isInitialized() const override {
        return true;
    }

    Result readDeviceInfo(SessionMetadata& metadataOut) override {
        metadataOut.deviceMac = "F9:CB:94:04:C3:45";
        metadataOut.serialNumber = "0561E1";
        metadataOut.firmwareRevision = "1.7.2";
        return Result::ok();
    }

    Result configureHeadMotionSignals(const SessionConfig&) override {
        configureCalled = true;
        return Result::ok();
    }

    Result createHeadMotionLoggers(SessionMetadata& metadataInOut) override {
        createLoggersCalled = true;
        metadataInOut.accelLoggerId = 0;
        metadataInOut.gyroLoggerId = 1;
        return Result::ok();
    }

    Result startLogging(bool) override {
        startLoggingCalled = true;
        return Result::ok();
    }

    Result stopLogging() override {
        return Result::ok();
    }

    Result flushLogPage() override {
        return Result::ok();
    }

    Result subscribeLoggers(
        const SessionMetadata&,
        std::function<void(const ImuSample&)>
    ) override {
        return Result::ok();
    }

    Result downloadLogs(
        std::function<void(const DownloadProgress&)>
    ) override {
        return Result::ok();
    }

    Result clearLogEntries() override {
        clearCalled = true;
        return Result::ok();
    }
};
```

Example test shape:

```cpp
TEST_CASE("record-start clears old logs when eraseBeforeStart is enabled") {
    FakeMetaWearBoard board;
    FakeMetadataStore metadata;
    TestLogger logger;

    RecordStartService service(board, metadata, logger);

    SessionConfig config;
    config.eraseBeforeStart = true;

    Result result = service.run("MetaWear", config);

    CHECK(result.ok());
    CHECK(board.connectCalled);
    CHECK(board.initializeCalled);
    CHECK(board.clearCalled);
    CHECK(board.configureCalled);
    CHECK(board.createLoggersCalled);
    CHECK(board.startLoggingCalled);
    CHECK(board.disconnectCalled);
    CHECK(metadata.saved.has_value());
}
```

---

## Recommended Development Sequence

```text
1. Keep the current working probe as a reference.
2. Create the new modular project structure.
3. Move SimpleBLE scanning/connection code into SimpleBleAdapter.
4. Move MblMwBtleConnection callbacks into MetaWearConnection.
5. Wrap board initialization and device info into MetaWearBoardAdapter.
6. Add core structs: Result, SessionConfig, SessionMetadata, ImuSample, DownloadProgress.
7. Add RecordStartService with fake-board tests.
8. Add SyncService with fake-board tests.
9. Only then wire real MetaWear logger creation/download into MetaWearBoardAdapter.
10. Add daemon/sync-station behavior after manual record-start/sync works.
```

---

## First Production Data Folder Design

Suggested output path:

```text
~/HeadMotion/data/
└── 0561E1/
    ├── 2026-05-18_090000.session.json
    ├── 2026-05-18_090000_accel.csv.partial
    ├── 2026-05-18_090000_gyro.csv.partial
    └── 2026-05-18_090000_download.log
```

After verified successful download:

```text
2026-05-18_090000_accel.csv.partial → 2026-05-18_090000_accel.csv
2026-05-18_090000_gyro.csv.partial  → 2026-05-18_090000_gyro.csv
```

Do not erase sensor logs until the `.partial` files have been flushed, closed, verified, and renamed.

---

## Near-Term Coding Goal

The next chat should probably start with:

```text
I am building HeadMotion, a modular C++ app on Arch Linux for the MbientLab MetaMotionS+ / MMS+ sensor. I already have a working probe that connects over SimpleBLE, initializes the MetaWear board with status=0, and reads device info. I want to refactor it into a highly unit-testable architecture. Please help me create the initial project skeleton with CMake, Makefile, core types, interfaces, fake test classes, and first RecordStartService tests.
```

Current known working device target:

```text
MetaWear
F9:CB:94:04:C3:45
serial 0561E1
firmware 1.7.2
```
