# Head Motion USB Client

Head Motion USB Client is a C++ command line application for communicating with an MbientLab MetaMotionS / MMS+ sensor over USB serial.

This project replaces the previous BLE-based client with a USB-first implementation. The goal is to bypass BLE for faster, more reliable command/control and data transfer.

## Project Goals

- Communicate with the MetaMotionS over USB serial.
- Use the MbientLab MetaWear C++ SDK where possible.
- Keep transport, protocol, SDK bridge, and application logic separated.
- Support Linux first, with a structure that can later support macOS and Windows.
- Provide a command line tool for sensor setup, recording, stopping, syncing, and debugging.
- Make the code modular and testable.

## Current Status

Working:

- Serial port discovery on Linux.
- Device identification over USB.
- Raw USB frame transmission.
- MetaWear module-info command over USB.
- MetaWear SDK initialization through the USB bridge.
- Record-start command for internal accelerometer and gyroscope logging.

In progress:

- Record-stop command.
- Data sync/download command.
- Cross-platform serial backends.
- Better session management and output file handling.

## Hardware

Tested with:

- MbientLab MetaMotionS / MMS+
- USB serial device exposed as `/dev/ttyACM0`
- Linux host

Known device information from testing:

```text
Device: MbientLab MetaMotionS
Model: 8
Hardware: 0.1
Firmware: 1.7.2
Board serial: 0561E1
USB serial: F9CB9404C345
```

## Architecture

The application is split into platform-independent and platform-specific layers.

### Platform-independent layers

- `headmotion_core`
  - Shared utilities.
  - Hex parsing and formatting.

- `headmotion_transport`
  - Abstract byte transport interfaces.
  - Serial configuration types.
  - Serial port discovery types.

- `headmotion_protocol`
  - USB frame encoding and decoding.
  - MetaMotionS USB frame format.

- `headmotion_metawear_usb`
  - Bridges framed USB communication into a MetaWear-style byte transport.

- `headmotion_sdk_bridge`
  - Connects the MetaWear C++ SDK to the USB transport.
  - Implements SDK callbacks for GATT-style reads, writes, and notifications.

- `headmotion_app`
  - CLI command implementations.

### Platform-specific layers

- `headmotion_platform_serial`
  - Native Linux serial implementation.
  - Uses termios, poll, read, write, and modem control lines.

## USB Frame Format

The USB command frame format currently used by the device is:

```text
1F <payload-length> <payload-bytes> 0A
```

Example payload:

```text
01 80
```

Full framed command:

```text
1F 02 01 80 0A
```

Example response:

```text
1F 04 01 80 00 00 0A
```

Decoded response payload:

```text
01 80 00 00
```

## Dependencies

Required:

- CMake 3.22 or newer
- C++20 compiler
- Ninja or Make
- Git
- MbientLab MetaWear SDK C++

Linux packages commonly needed:

```bash
sudo pacman -S base-devel cmake ninja git
```

On Debian/Ubuntu-style systems:

```bash
sudo apt install build-essential cmake ninja-build git
```

## MetaWear SDK

This project expects the MetaWear SDK C++ repository to exist locally.

Current default path:

```text
/home/kyle/Collage/HeadMotion/code/MetaWear-SDK-Cpp
```

This path is configured in `CMakeLists.txt`:

```cmake
set(METAWEAR_SDK_DIR
    "/home/kyle/Collage/HeadMotion/code/MetaWear-SDK-Cpp"
    CACHE PATH
    "Path to MetaWear-SDK-Cpp"
)
```

If the SDK lives somewhere else, configure with:

```bash
cmake -S . -B build/linux-native-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHEADMOTION_SERIAL_BACKEND=native \
  -DMETAWEAR_SDK_DIR=/path/to/MetaWear-SDK-Cpp
```

## Build

From the project root:

```bash
make build
```

The binary will be created at:

```text
build/linux-native-debug/mmsctl
```

Manual CMake build:

```bash
cmake -S . -B build/linux-native-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHEADMOTION_SERIAL_BACKEND=native

cmake --build build/linux-native-debug
```

## Usage

```text
mmsctl scan
mmsctl identify <serial-port>
mmsctl tx-raw <serial-port> <hex-bytes>
mmsctl cmd <serial-port> <payload-hex>
mmsctl module-info <serial-port>
mmsctl sdk-probe <serial-port>
mmsctl record-start <serial-port> [--rate 25|50|100]
```

## Commands

### Scan for serial devices

```bash
./build/linux-native-debug/mmsctl scan
```

Example output:

```text
Serial ports:
  /dev/ttyACM0  via /dev/serial/by-id/usb-MbientLab_MetaMotionS_F9CB9404C345-if00  [usb-MbientLab_MetaMotionS_F9CB9404C345-if00]  likely MMS
```

### Identify the sensor

```bash
./build/linux-native-debug/mmsctl identify /dev/ttyACM0
```

Example output:

```text
Opening /dev/ttyACM0
Sending identity query: ?\n
Response:
MbientLab MetaMotionS 8 0.1 1.7.2 0561E1
```

### Send a raw USB frame

```bash
./build/linux-native-debug/mmsctl tx-raw /dev/ttyACM0 "1F 02 01 80 0A"
```

Example output:

```text
Opening /dev/ttyACM0
TX [5 bytes]: 1F 02 01 80 0A
RX [7 bytes] hex:
1F 04 01 80 00 00 0A
RX ASCII preview:
......\n
```

### Send a payload command

This automatically wraps the payload in the USB frame format.

```bash
./build/linux-native-debug/mmsctl cmd /dev/ttyACM0 "01 80"
```

Example output:

```text
Opening /dev/ttyACM0
Payload TX [2 bytes]: 01 80
Framed TX [5 bytes]: 1F 02 01 80 0A
Raw RX [7 bytes]:
1F 04 01 80 00 00 0A
Frame 0 payload [4 bytes]: 01 80 00 00
```

### Read module info

```bash
./build/linux-native-debug/mmsctl module-info /dev/ttyACM0
```

Example output:

```text
Opening /dev/ttyACM0
Sending module-info payload: 01 80
Module-info response payload [4 bytes]: 01 80 00 00
Module-info round trip OK.
```

### Probe SDK initialization

```bash
./build/linux-native-debug/mmsctl sdk-probe /dev/ttyACM0
```

This initializes the MetaWear SDK board object through the USB bridge.

Successful output should end with something similar to:

```text
SDK initialized callback status=0
SDK probe initialized=true status=0
```

### Start internal recording

Default rate is 50 Hz:

```bash
./build/linux-native-debug/mmsctl record-start /dev/ttyACM0
```

Explicit rate:

```bash
./build/linux-native-debug/mmsctl record-start /dev/ttyACM0 --rate 25
./build/linux-native-debug/mmsctl record-start /dev/ttyACM0 --rate 50
./build/linux-native-debug/mmsctl record-start /dev/ttyACM0 --rate 100
```

Unsupported rates are rejected:

```bash
./build/linux-native-debug/mmsctl record-start /dev/ttyACM0 --rate 800
```

Expected output:

```text
ERROR: Unsupported sample rate. Use one of: 25, 50, 100 Hz
```

## Makefile Targets

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

Send raw frame:

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

Clean:

```bash
make clean
```

Remove the build directory:

```bash
make distclean
```

## Linux Serial Permissions

If the device appears as `/dev/ttyACM0` but cannot be opened, check permissions:

```bash
ls -l /dev/ttyACM0
```

On many Linux systems, the user needs to be in the `uucp`, `dialout`, or similar serial group.

Arch Linux commonly uses `uucp`:

```bash
sudo usermod -aG uucp "$USER"
```

Debian/Ubuntu commonly uses `dialout`:

```bash
sudo usermod -aG dialout "$USER"
```

Log out and log back in after changing group membership.

## Development Notes

The project currently uses the native Linux serial backend.

The namespace for the Linux backend is:

```cpp
headmotion::platform::linux_platform
```

Do not use this namespace:

```cpp
headmotion::platform::linux
```

Some Linux systems define `linux` as a macro, which can break compilation.

## Known Working Device Behavior

The MetaMotionS responds to the text identity query:

```text
?\n
```

Example response:

```text
MbientLab MetaMotionS 8 0.1 1.7.2 0561E1
```

The module-info payload:

```text
01 80
```

Returns:

```text
01 80 00 00
```

SDK initialization currently works by responding to SDK GATT-style reads with known device metadata and by routing SDK writes through USB frames.

## Roadmap

Planned next commands:

```text
record-stop
sync
status
clear
session-list
```

Planned features:

- Stop accel and gyro sampling cleanly.
- Stop internal logging.
- Download logged data from the sensor.
- Save downloaded samples to CSV.
- Add resumable sync.
- Add session metadata.
- Add macOS serial backend.
- Add Windows serial backend.
- Add unit tests for frame parsing and command construction.

## Git Workflow

This repository replaced the old BLE implementation.

The old BLE code should be preserved on a branch such as:

```text
old-ble-code
```

The new USB client should live on:

```text
main
```

Recommended push command when replacing the old remote main branch:

```bash
git push --force-with-lease origin main
```

## License

Add project license information here.
