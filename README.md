# Head Motion USB Client

A C++ command-line application for recording and downloading data from an MbientLab MetaMotionS / MMS+ sensor over USB serial.

The application supports the complete internal logging workflow:

1. Discover and save the connected MMS+ serial port.
2. Clear old logger configuration.
3. Start accelerometer, gyroscope, and optional battery logging.
4. Stop recording.
5. Download recorded data into CSV files.

## Platform Support

Currently supported:

* Linux

Planned:

* Windows 11

## Features

* MMS+ USB serial discovery and verification
* Saved default serial-port selection
* Stable device matching using USB metadata
* Manual `--port` override
* MetaWear SDK initialization over USB
* Internal accelerometer and gyroscope logging
* Optional battery-state logging
* Recording start, stop, reset, and sync commands
* Append-only CSV writing
* Automatic collision-free CSV filenames

## Installation

### Install dependencies

Arch Linux:

```bash
sudo pacman -S base-devel cmake ninja git
```

Debian or Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build git
```

### Clone the repository

```bash
git clone https://github.com/KylesCorner/Head_Motion.git
cd Head_Motion
```

### Install the MetaWear SDK

From the repository root:

```bash
mkdir -p external
git clone https://github.com/mbientlab/MetaWear-SDK-Cpp.git \
    external/MetaWear-SDK-Cpp

cd external/MetaWear-SDK-Cpp
git submodule update --init --recursive
cd ../..
```

### Configure and build

```bash
cmake -S . -B build/linux-native-debug -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DHEADMOTION_SERIAL_BACKEND=native \
    -DMETAWEAR_SDK_DIR="$PWD/external/MetaWear-SDK-Cpp"

cmake --build build/linux-native-debug
```

The executable will be located at:

```text
build/linux-native-debug/mmsctl
```

## Serial Permissions

The MMS+ normally appears as `/dev/ttyACM*`.

On Arch Linux:

```bash
sudo usermod -aG uucp "$USER"
```

On Debian or Ubuntu:

```bash
sudo usermod -aG dialout "$USER"
```

Log out and log back in after changing group membership.

## Device Discovery

Connect the MMS+ and run:

```bash
./build/linux-native-debug/mmsctl scan
```

The scan command identifies the MMS+, verifies it over USB, and saves it as the default device.

The saved device record is stored at:

```text
data/latest_device_port.bin
```

After scanning, commands can use the saved device without specifying a serial port.

An explicit port can still be supplied when needed:

```bash
./build/linux-native-debug/mmsctl identify \
    --port /dev/ttyACM0
```

## Usage

General format:

```text
mmsctl <command> [options]
```

| Command        | Usage                                                                        | Description                                        |
| -------------- | ---------------------------------------------------------------------------- | -------------------------------------------------- |
| `scan`         | `mmsctl scan`                                                                | Discover, verify, and save the default MMS+ device |
| `identify`     | `mmsctl identify [--port PORT]`                                              | Read device identity information                   |
| `module-info`  | `mmsctl module-info [--port PORT]`                                           | Read MetaWear module information                   |
| `sdk-probe`    | `mmsctl sdk-probe [--port PORT]`                                             | Test MetaWear SDK initialization                   |
| `tx-raw`       | `mmsctl tx-raw [--port PORT] HEX`                                            | Send a complete raw USB frame                      |
| `cmd`          | `mmsctl cmd [--port PORT] PAYLOAD`                                           | Send a MetaWear payload using USB framing          |
| `record-reset` | `mmsctl record-reset [--port PORT]`                                          | Clear existing logger configuration                |
| `record-start` | `mmsctl record-start [--port PORT] [--rate HZ] [--battery-interval SECONDS]` | Start internal recording                           |
| `record-stop`  | `mmsctl record-stop [--port PORT]`                                           | Stop sampling and internal logging                 |
| `sync`         | `mmsctl sync [--port PORT] [--out DIRECTORY]`                                | Download logged data into CSV files                |

Supported sample rates:

```text
25, 50, 100, 200, 400, 800, 1600, 3200 Hz
```

The default sample rate is `50 Hz`.

Battery logging is disabled unless `--battery-interval` is provided.

## Full Recording Workflow

First discover and save the MMS+ port:

```bash
./build/linux-native-debug/mmsctl scan
```

Reset any previous logger configuration:

```bash
./build/linux-native-debug/mmsctl record-reset
sleep 3
```

Start recording:

```bash
./build/linux-native-debug/mmsctl record-start --rate 50
```

Wear or move the sensor while it records internally.

Stop recording:

```bash
./build/linux-native-debug/mmsctl record-stop
```

Download the recorded data:

```bash
./build/linux-native-debug/mmsctl sync \
    --out data/session_001
```

The downloaded IMU data is written to:

```text
data/session_001/imu.csv
```

The IMU CSV format is:

```csv
epoch_ms,sensor,x,y,z
```

## Battery Logging

Start a recording with battery state sampled every 60 seconds:

```bash
./build/linux-native-debug/mmsctl record-reset
sleep 3

./build/linux-native-debug/mmsctl record-start \
    --rate 25 \
    --battery-interval 60
```

Stop and download the recording:

```bash
./build/linux-native-debug/mmsctl record-stop

./build/linux-native-debug/mmsctl sync \
    --out data/battery_test_001
```

This produces:

```text
data/battery_test_001/imu.csv
data/battery_test_001/battery.csv
```

The battery CSV format is:

```csv
epoch_ms,voltage_mv,charge_percent
```

## Data Integrity

The sync command does not overwrite existing CSV files.

When the output directory already contains data, the next unused filename is selected:

```text
imu.csv
imu_1.csv
imu_2.csv
```

Battery files use the matching number:

```text
battery.csv
battery_1.csv
battery_2.csv
```

CSV streams are opened in append mode as an additional safeguard against accidental truncation.

## License

To be added.
