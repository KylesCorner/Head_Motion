# HeadMotion USB Client

HeadMotion is a C++ application for controlling and downloading data from an
MbientLab MetaMotionS / MMS+ sensor over USB.

The project is designed around the MMS+ internal logging workflow. The sensor
can be configured from a computer, used to record motion internally, and later
reconnected to download accelerometer, gyroscope, and optional battery data.

HeadMotion provides two interfaces:

- **HeadMotion GUI** — lightweight FLTK interface for normal recording and
  download workflows.
- **`mmsctl` CLI** — command-line interface for development, diagnostics,
  scripting, and lower-level device operations.

## Platform Support

Currently supported:

- Linux x86-64

Planned:

- Windows 11

## Features

- MMS+ USB serial discovery and verification
- Saved default device selection
- Stable device matching using USB metadata
- Manual `--port` override
- MetaWear SDK communication over USB
- Internal accelerometer logging
- Internal gyroscope logging
- Optional battery-state logging
- Configurable IMU sample rates
- Recording start, stop, reset, and download operations
- FLTK graphical interface
- Command-line diagnostic tools
- Download progress reporting
- Collision-free CSV filenames
- Portable Linux AppImage distribution

---

# Installation

## Linux AppImage

For normal use, the recommended installation method is the prebuilt AppImage
available from the GitHub Releases page.

Download the latest:

```text
HeadMotion-<version>-Linux.AppImage
```

Make it executable:

```bash
chmod +x HeadMotion-*-Linux.AppImage
```

Run it:

```bash
./HeadMotion-*-Linux.AppImage
```

The AppImage is portable and does not require the HeadMotion source tree,
CMake, Ninja, FLTK development packages, or the MetaWear SDK source.

---

# Developer Build

Developers can build HeadMotion from source using the included Makefile.

The developer build produces:

```text
mmsctl
headmotion_gui
```

## 1. Install dependencies

### Arch Linux

```bash
sudo pacman -S base-devel cmake ninja git fltk
```

### Debian / Ubuntu

```bash
sudo apt install \
    build-essential \
    cmake \
    ninja-build \
    git \
    libfltk1.3-dev
```

## 2. Clone HeadMotion

```bash
git clone https://github.com/KylesCorner/Head_Motion.git
cd Head_Motion
```

## 3. Install the MetaWear SDK

Clone the MetaWear C++ SDK into the `external` directory:

```bash
mkdir -p external

git clone https://github.com/mbientlab/MetaWear-SDK-Cpp.git \
    external/MetaWear-SDK-Cpp
```

Initialize its submodules:

```bash
cd external/MetaWear-SDK-Cpp
git submodule update --init --recursive
cd ../..
```

The repository should now look approximately like:

```text
Head_Motion/
├── external/
│   └── MetaWear-SDK-Cpp/
├── include/
├── packaging/
├── src/
├── CMakeLists.txt
└── Makefile
```

## 4. Build Debug

```bash
make debug
```

The Debug executables are created at:

```text
build/linux-native-debug/mmsctl
build/linux-native-debug/headmotion_gui
```

Run the GUI:

```bash
make run-gui-debug
```

## 5. Build Release

```bash
make release
```

The optimized Release executables are created at:

```text
build/linux-native-release/mmsctl
build/linux-native-release/headmotion_gui
```

Run the Release GUI:

```bash
make run-gui-release
```

---

# Makefile Commands

The included Makefile provides shortcuts for normal development tasks.

| Command | Description |
| --- | --- |
| `make debug` | Configure and build the Debug version |
| `make release` | Configure and build the Release version |
| `make rebuild-debug` | Clean and rebuild Debug |
| `make rebuild-release` | Clean and rebuild Release |
| `make run-gui-debug` | Build and launch the Debug GUI |
| `make run-gui-release` | Build and launch the Release GUI |
| `make test-debug` | Build and run Debug tests |
| `make test-release` | Build and run Release tests |
| `make appimage` | Build Release and create the Linux AppImage |
| `make clean-debug` | Clean the Debug build |
| `make clean-release` | Clean the Release build |
| `make clean` | Clean both builds |
| `make distclean` | Remove the entire `build/` directory |
| `make help` | Show available Makefile commands |

---

# Building the AppImage

Create a distributable Linux AppImage with:

```bash
make appimage
```

This automatically:

1. Configures the Release build.
2. Builds HeadMotion in Release mode.
3. Clears the previous CPack staging directory.
4. Runs the AppImage CPack generator.

The finished AppImage is written to:

```text
build/linux-native-release/
```

For example:

```text
build/linux-native-release/HeadMotion-0.1.0-Linux.AppImage
```

That file can be distributed directly to Linux users.

---

# Serial Permissions

The MMS+ normally appears as a device such as:

```text
/dev/ttyACM0
```

Your user account must have permission to access the serial device.

## Arch Linux

```bash
sudo usermod -aG uucp "$USER"
```

## Debian / Ubuntu

```bash
sudo usermod -aG dialout "$USER"
```

Log out and log back in after changing group membership.

You can verify the device with:

```bash
ls -l /dev/ttyACM*
```

---

# Typical Use Cases

| Use case | Interface | Example |
| --- | --- | --- |
| Record a motion session | GUI | Scan → Reset → Start → Stop → Download |
| Download an existing recording | GUI | Scan → Select output directory → Download |
| Select an IMU sample rate | GUI | Choose rate before starting recording |
| Verify the connected MMS+ | CLI | `make run-scan` |
| Read device identity | CLI | `make run-identify PORT=/dev/ttyACM0` |
| Start a test recording | CLI | `make run-record-start PORT=/dev/ttyACM0 RATE=200` |
| Stop a test recording | CLI | `make run-record-stop PORT=/dev/ttyACM0` |
| Download a test recording | CLI | `make run-sync PORT=/dev/ttyACM0 OUT=data/session_001` |
| Clear logger state | CLI | `make run-record-reset PORT=/dev/ttyACM0` |
| Build a portable release | Developer | `make appimage` |

---

# GUI Recording Workflow

A typical recording session can be performed entirely from the graphical
application.

Launch the Debug GUI:

```bash
make run-gui-debug
```

or the optimized Release GUI:

```bash
make run-gui-release
```

Then:

1. Connect the MMS+ over USB.
2. Select **Scan Device**.
3. Choose the desired sample rate.
4. Select **Reset Loggers**.
5. Select **Start Recording**.
6. Perform the motion experiment.
7. Select **Stop Recording**.
8. Choose an output directory.
9. Select **Download Recording**.

The GUI displays download progress while logger entries are transferred from
the MMS+ and the CSV files are finalized.

---

# CLI Development Workflow

The Makefile also provides wrappers around the most common `mmsctl` commands.

## Scan for the MMS+

```bash
make run-scan
```

The scan command discovers and verifies the MMS+ and saves it as the default
device.

## Identify a device

```bash
make run-identify PORT=/dev/ttyACM0
```

## Reset existing loggers

```bash
make run-record-reset PORT=/dev/ttyACM0
```

Allow the board a few seconds to finish resetting before starting a new
recording.

## Start recording

At the default 50 Hz:

```bash
make run-record-start PORT=/dev/ttyACM0
```

At 200 Hz:

```bash
make run-record-start \
    PORT=/dev/ttyACM0 \
    RATE=200
```

## Stop recording

```bash
make run-record-stop PORT=/dev/ttyACM0
```

## Download recording

```bash
make run-sync \
    PORT=/dev/ttyACM0 \
    OUT=data/session_001
```

The resulting IMU data is written under:

```text
data/session_001/
```

---

# Direct `mmsctl` Usage

The CLI can also be invoked directly.

General format:

```text
mmsctl <command> [options]
```

| Command | Usage | Description |
| --- | --- | --- |
| `scan` | `mmsctl scan` | Discover, verify, and save the default MMS+ |
| `identify` | `mmsctl identify [--port PORT]` | Read device identity information |
| `module-info` | `mmsctl module-info [--port PORT]` | Read MetaWear module information |
| `sdk-probe` | `mmsctl sdk-probe [--port PORT]` | Test MetaWear SDK initialization |
| `tx-raw` | `mmsctl tx-raw [--port PORT] HEX` | Send a complete raw USB frame |
| `cmd` | `mmsctl cmd [--port PORT] PAYLOAD` | Send a MetaWear payload using USB framing |
| `record-reset` | `mmsctl record-reset [--port PORT]` | Clear existing logger configuration |
| `record-start` | `mmsctl record-start [--port PORT] [--rate HZ] [--battery-interval SECONDS]` | Start internal recording |
| `record-stop` | `mmsctl record-stop [--port PORT]` | Stop sampling and logging |
| `sync` | `mmsctl sync [--port PORT] [--out DIRECTORY]` | Download logged data |

Supported sample rates:

```text
25
50
100
200
400
800
1600
3200 Hz
```

The default sample rate is:

```text
50 Hz
```

Battery logging is disabled unless `--battery-interval` is provided.

---

# Device Discovery

HeadMotion can save a verified MMS+ as the default device.

Using the Makefile:

```bash
make run-scan
```

or directly:

```bash
./build/linux-native-debug/mmsctl scan
```

The saved device record is stored at:

```text
data/latest_device_port.bin
```

Once a device has been saved, `mmsctl` commands can resolve it automatically.

An explicit serial port can still be provided when required:

```bash
./build/linux-native-debug/mmsctl identify \
    --port /dev/ttyACM0
```

---

# Battery Logging

Battery logging can be enabled using the direct `mmsctl` interface.

Reset the loggers:

```bash
./build/linux-native-debug/mmsctl record-reset
sleep 3
```

Start IMU logging at 25 Hz and battery logging every 60 seconds:

```bash
./build/linux-native-debug/mmsctl record-start \
    --rate 25 \
    --battery-interval 60
```

Stop recording:

```bash
./build/linux-native-debug/mmsctl record-stop
```

Download:

```bash
./build/linux-native-debug/mmsctl sync \
    --out data/battery_test_001
```

This produces:

```text
data/battery_test_001/
├── imu.csv
└── battery.csv
```

---

# Output Data

## IMU CSV

The IMU CSV format is:

```text
epoch_ms,sensor,x,y,z
```

Example:

```text
1786651094161,accel_g,0.409790,-0.911499,0.207886
1786651094165,accel_g,0.411743,-0.927856,0.207153
1786651094161,gyro_dps,2.136,-0.427,1.282
```

The `sensor` field identifies the measurement type:

```text
accel_g
gyro_dps
```

## Battery CSV

When battery logging is enabled:

```text
epoch_ms,voltage_mv,charge_percent
```

Example:

```text
1786651094161,4181,100
```

---

# Data Integrity

HeadMotion avoids overwriting previous recording downloads.

If an output directory already contains:

```text
imu.csv
```

the next download uses:

```text
imu_1.csv
```

followed by:

```text
imu_2.csv
imu_3.csv
...
```

Battery files use the corresponding index:

```text
battery.csv
battery_1.csv
battery_2.csv
...
```

CSV streams are opened in append mode as an additional safeguard against
accidental truncation.

---

# Project Structure

HeadMotion separates high-level application behavior from the USB protocol and
platform-specific serial implementation.

```text
src/
├── app/            Application commands
├── gui/            FLTK graphical interface
├── metawear/       MetaWear USB transport
├── platform/       OS-specific serial implementations
├── protocol/       MMS+ USB framing
├── sdk/            MetaWear SDK bridge
├── session/        Persistent device/session state
└── util/           Portable utilities
```

This structure allows additional platform backends, including Windows, to be
added without changing the higher-level recording workflow.

---

# License

To be added.