# Head Motion USB Client

## Project Summary

Head Motion USB Client is a C++ command line application for communicating with an MbientLab MetaMotionS / MMS+ sensor over USB serial.

This project replaces the older BLE-based client with a USB-first implementation. The purpose of the project is to support the full sensor recording workflow over USB:
1. Clear any old recording loggers on MetaMotionS.
2. Start internal recording.
3. Stop internal recording cleanly.
4. Sync/download the recorded accelerometer and gyroscope data.
5. Save the downloaded data to files for analysis.

The current implementation can already communicate with the device over USB, initialize the MetaWear SDK through the USB bridge, and start internal accelerometer and gyroscope logging.

Current supported platform:

- Linux

Current working features:

- Scan for connected serial devices
- Identify the MetaMotionS over USB
- Send raw USB frames
- Send framed MetaWear command payloads
- Read MetaWear module info
- Initialize the MetaWear SDK through the USB bridge
- Start internal accelerometer and gyroscope logging
- Record start and stop commands over a wire
- Sync data into a csv fromat over a wire

Required next features:

- Compatability with Windows terminal environment


## Installation

### 1. Install system dependencies

On Arch Linux:

```bash
sudo pacman -S base-devel cmake ninja git
```

On Debian or Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build git
```

### 2. Clone this repository

```bash
git clone https://github.com/KylesCorner/Head_Motion.git
cd Head_Motion
```

### 3. MetaWear SDK installation

From inside the project repo root
```bash
mkdir -p external && \
cd external && \
rm -rf MetaWear-SDK-Cpp && \
git clone https://github.com/mbientlab/MetaWear-SDK-Cpp.git && \
cd MetaWear-SDK-Cpp && \
git submodule update --init --recursive && \
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
cmake --build build

cmake -S . -B build/linux-native-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHEADMOTION_SERIAL_BACKEND=native \
  -DMETAWEAR_SDK_DIR="$PWD/external/MetaWear-SDK-Cpp" && \
cmake --build build/linux-native-debug
```

### 4. Build

Using the included Makefile:

```bash
make build
```

The compiled binary will be located at:

```text
build/linux-native-debug/mmsctl
```

### 5. Serial permissions

If the sensor appears as `/dev/ttyACM0` but cannot be opened, add your user to the serial device group.

On Arch Linux:

```bash
sudo usermod -aG uucp "$USER"
```

On Debian or Ubuntu:

```bash
sudo usermod -aG dialout "$USER"
```

Log out and log back in after changing group membership.
## Usage

Command Table:

| Command        | Usage                                                                                                            | Arguments                                                                                                                                                                                                           |
| -------------- | ---------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `scan`         | `mmsctl scan`                                                                                                    | None                                                                                                                                                                                                                |
| `identify`     | `mmsctl identify <serial-port>`                                                                                  | `<serial-port>`: serial device path, for example `/dev/ttyACM0`                                                                                                                                                     |
| `tx-raw`       | `mmsctl tx-raw <serial-port> <hex-bytes>`                                                                        | `<serial-port>`: serial device path. `<hex-bytes>`: full raw USB frame bytes, for example `"1F 02 01 80 0A"`                                                                                                        |
| `cmd`          | `mmsctl cmd <serial-port> <payload-hex>`                                                                         | `<serial-port>`: serial device path. `<payload-hex>`: MetaWear command payload only; the app wraps it in the USB frame format, for example `"01 80"`                                                                |
| `module-info`  | `mmsctl module-info <serial-port>`                                                                               | `<serial-port>`: serial device path                                                                                                                                                                                 |
| `sdk-probe`    | `mmsctl sdk-probe <serial-port>`                                                                                 | `<serial-port>`: serial device path                                                                                                                                                                                 |
| `record-reset` | `mmsctl record-reset <serial-port>`                                                                              | `<serial-port>`: serial device path. Clears existing onboard logging/routes so a fresh recording session can be configured                                                                                          |
| `record-start` | `mmsctl record-start <serial-port> [--rate 25\|50\|100\|200\|400\|800\|1600\|3200] [--battery-interval seconds]` | `<serial-port>`: serial device path. `--rate`: optional accel/gyro sample rate in Hz; defaults to `50`. `--battery-interval`: optional battery logging interval in seconds. If omitted, battery logging is disabled |
| `record-stop`  | `mmsctl record-stop <serial-port>`                                                                               | `<serial-port>`: serial device path. Stops accel/gyro sampling, stops onboard logging, and prepares the board for sync                                                                                              |
| `sync`         | `mmsctl sync <serial-port> <output-dir>`                                                                         | `<serial-port>`: serial device path. `<output-dir>`: directory where downloaded CSV files will be saved                                                                                                             |

General command format:

```text
mmsctl <command> [arguments]
```

The binary path after building is:

```text
./build/linux-native-debug/mmsctl
```

## Full Recording Workflow

The normal accelerometer + gyroscope recording workflow is:

```bash
./build/linux-native-debug/mmsctl scan
./build/linux-native-debug/mmsctl record-reset /dev/ttyACM0
sleep 3
./build/linux-native-debug/mmsctl record-start /dev/ttyACM0 --rate 50

# Wear or move the sensor while it records internally.

./build/linux-native-debug/mmsctl record-stop /dev/ttyACM0
./build/linux-native-debug/mmsctl sync /dev/ttyACM0 --out data/session_001
```

This produces:

```text
data/session_001/imu.csv
```

For an accelerometer + gyroscope + battery logging session, enable battery logging with `--battery-interval`:

```bash
./build/linux-native-debug/mmsctl scan
./build/linux-native-debug/mmsctl record-reset /dev/ttyACM0
sleep 3
./build/linux-native-debug/mmsctl record-start /dev/ttyACM0 --rate 25 --battery-interval 60

# Unplug the sensor and run the battery-life / head-motion experiment.
# Battery state will be sampled once every 60 seconds.

./build/linux-native-debug/mmsctl record-stop /dev/ttyACM0
./build/linux-native-debug/mmsctl sync /dev/ttyACM0 --out data/battery_test_001
```

This produces:

```text
data/battery_test_001/imu.csv
data/battery_test_001/battery.csv
```

`imu.csv` contains accelerometer and gyroscope rows:

```csv
epoch_ms,sensor,x,y,z
```

`battery.csv` contains battery voltage and charge rows:

```csv
epoch_ms,voltage_mv,charge_percent
```

Battery logging is optional. If `--battery-interval` is not provided to `record-start`, no battery logger or battery timer is created, and `sync` only writes `imu.csv`.

## Makefile Shortcuts

Build:

```bash
make build
```

Scan:

```bash
make run-scan
```

Identify:

```bash
make run-identify PORT=/dev/ttyACM0
```

Send a raw frame:

```bash
make run-tx-raw PORT=/dev/ttyACM0 HEX="1F 02 01 80 0A"
```

Read module info:

```bash
make run-module-info PORT=/dev/ttyACM0
```

Start recording:

```bash
make run-record-start PORT=/dev/ttyACM0 RATE=50
```

Planned stop recording shortcut:

```bash
make run-record-stop PORT=/dev/ttyACM0
```

Planned sync shortcut:

```bash
make run-sync PORT=/dev/ttyACM0 OUT=data/session_001
```

Clean build artifacts:

```bash
make clean
```

Remove the build directory:

```bash
make distclean
```

## License
To be added
