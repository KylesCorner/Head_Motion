# HeadMotion USB Client

HeadMotion is a C++ application for controlling and downloading data from
MbientLab MetaMotionS / MMS+ sensors over USB.

The application is built around the MMS+ internal logging workflow:

1. Connect the sensor over USB.
2. Configure and start onboard accelerometer/gyroscope logging.
3. Disconnect the sensor and perform the recording.
4. Reconnect it later.
5. Stop recording and download the logged data.

HeadMotion provides two interfaces:

- **HeadMotion GUI** — the normal interface for recording and downloading data.
- **`mmsctl` CLI** — command-line control for scripting, diagnostics, and lower-level device access.

For source builds, testing, packaging, and development setup, see
[DEVELOPMENT.md](DEVELOPMENT.md).

---

## Installation

Prebuilt releases are available from:

https://github.com/KylesCorner/Head_Motion/releases

### Linux x86-64

Download the latest Linux AppImage:

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

The AppImage contains the application and its runtime dependencies. A source
checkout, compiler, CMake, FLTK development packages, and MetaWear SDK source
are not required for normal use.

### Linux serial permissions

The MMS+ normally appears as a serial device such as:

```text
/dev/ttyACM0
```

Your user account must have permission to access the serial port.

On Arch Linux:

```bash
sudo usermod -aG uucp "$USER"
```

On Debian/Ubuntu:

```bash
sudo usermod -aG dialout "$USER"
```

Log out and back in after changing group membership.

Verify that the device is present with:

```bash
ls -l /dev/ttyACM*
```

### Windows 11 x64

Download the Windows x64 executable/installer from the GitHub Releases page and
run it normally.

The MMS+ appears as a Windows COM device. HeadMotion uses the native Windows
serial backend and does not require a Linux compatibility layer.

### Building from source

Source builds are documented separately in:

[DEVELOPMENT.md](DEVELOPMENT.md)

---

## GUI Quick Start

A normal recording session can be completed entirely from the GUI.

1. Connect one or more MMS+ sensors over USB.
2. Click **Scan**.
3. Click **Reset** or **Reset All** before beginning a fresh recording.
4. Choose the desired sample rate.
5. Click **Start** or **Record Start All**.
6. Perform the recording.
7. Reconnect the sensor if it was disconnected during the session.
8. Click **Stop** or **Record Stop All**.
9. Choose an output directory.
10. Enable **Legacy CSV** if the long-format IMU CSV is also required.
11. Click **Sync** or **Sync All**.

The GUI displays per-device state and aggregate download progress while data is
transferred from onboard flash.

> **Important:** Resetting a recording clears the existing logger state. Sync
> any data you need before starting a fresh session.

---

## Command-Line Usage

The CLI executable is named:

```text
mmsctl
```

Examples below assume `mmsctl` is on your `PATH`. For a development build, the
Linux Debug executable is normally:

```text
./build/linux-native-debug/mmsctl
```

### Scan for devices

```bash
mmsctl scan
```

### Identify a device

By serial port:

```bash
mmsctl identify --port /dev/ttyACM0
```

By MMS+ device ID:

```bash
mmsctl identify --device-id 056D8D
```

### Reset recording state

```bash
mmsctl record-reset --port /dev/ttyACM0
```

### Start recording

At 200 Hz:

```bash
mmsctl record-start \
    --port /dev/ttyACM0 \
    --rate 200
```

With battery logging every 60 seconds:

```bash
mmsctl record-start \
    --port /dev/ttyACM0 \
    --rate 200 \
    --battery-interval 60
```

### Stop recording

```bash
mmsctl record-stop --port /dev/ttyACM0
```

### Download a recording

```bash
mmsctl sync \
    --port /dev/ttyACM0 \
    --out data/session_001
```

By default, sync writes the Xsens-style IMU CSV.

To also write the legacy long-format IMU CSV:

```bash
mmsctl sync \
    --port /dev/ttyACM0 \
    --out data/session_001 \
    --imu-csv
```

### JSON output

Commands that support normal CLI output can also emit machine-readable JSON:

```bash
mmsctl scan --json
```

```bash
mmsctl record-start \
    --port /dev/ttyACM0 \
    --rate 1600 \
    --json
```

---

## CLI Command Reference

| Command | Purpose |
| --- | --- |
| `scan` | Discover and verify connected MMS+ devices |
| `identify` | Read device identity information |
| `module-info` | Read MetaWear module information |
| `sdk-probe` | Test MetaWear SDK initialization |
| `record-reset` | Clear existing logger configuration before a fresh recording |
| `record-start` | Configure sensors and start internal logging |
| `record-stop` | Stop sensor sampling and internal logging |
| `sync` | Download logged data to CSV |
| `cmd` | Send a MetaWear command payload through USB framing |
| `tx-raw` | Send a complete raw USB frame |

Common device selectors:

```text
--port <serial-port>
--device-id <device-id>
```

Common output options:

```text
--json
--quiet
```

Sync options:

```text
--out <directory>
--output-dir <directory>
--imu-csv
```

---

## Sample Rates

`record-start --rate` accepts:

```text
25
50
100
200
400
800
1600
3200
```

The MetaMotionS uses a Bosch BMI270 IMU. The accelerometer and gyroscope do not
have identical maximum ODRs:

| Sensor | Maximum BMI270 ODR used by HeadMotion |
| --- | ---: |
| Accelerometer | 1600 Hz |
| Gyroscope | 3200 Hz |

A request for `--rate 3200` therefore requests 3200 Hz from the gyroscope while
the accelerometer is limited to 1600 Hz.

The 1600 Hz recording path has been tested on MMS+ hardware. The 3200 Hz gyro
mode should currently be treated as experimental and validated for the specific
recording workflow before relying on it for production data.

Higher sample rates also consume onboard logger storage much faster.

---

## Output Files

HeadMotion uses the MMS+ hardware/device ID in output filenames.

### Default IMU CSV

A normal sync writes:

```text
imu_<DEVICE_ID>.csv
```

This is the Xsens-style combined accelerometer/gyroscope output:

```text
PacketCounter,SampleTimeFine,Euler_X,Euler_Y,Euler_Z,Acc_X,Acc_Y,Acc_Z,Gyr_X,Gyr_Y,Gyr_Z,elapsed_ms,utc_timestamp
```

### Legacy IMU CSV

When **Legacy CSV** is enabled in the GUI, or `--imu-csv` is passed to
`mmsctl sync`, HeadMotion also writes:

```text
imu_legacy_<DEVICE_ID>.csv
```

Format:

```text
epoch_ms,elapsed_ms,sensor,x,y,z
```

The `sensor` field is either:

```text
accel_g
gyro_dps
```

This format preserves accelerometer and gyroscope samples independently and is
useful for sample-rate validation and low-level analysis.

### Battery CSV

If battery logging was enabled when the recording started, sync also writes:

```text
battery_<DEVICE_ID>.csv
```

Format:

```text
epoch_ms,elapsed_ms,voltage_mv,charge_percent
```

### Existing files

HeadMotion does not overwrite an existing recording. If a filename already
exists, a numeric suffix is added:

```text
imu_056D8D.csv
imu_056D8D_1.csv
imu_056D8D_2.csv
```

The corresponding legacy and battery files use the same collision-avoidance
scheme.

---

## Compatibility

### Hardware

| Hardware | Status |
| --- | --- |
| MbientLab MetaMotionS / MMS+ | Supported |
| BMI270 accelerometer + gyroscope | Supported |
| Other MetaWear boards | Not currently guaranteed |

HeadMotion is specifically designed around the MMS+ USB CDC transport and its
internal logging workflow.

### Operating systems

| Platform | Status | Notes |
| --- | --- | --- |
| Linux x86-64 | Supported | AppImage and native source builds |
| Windows 11 x64 | Supported | Native Windows serial backend |
| macOS | Not supported | Native macOS serial backend is not implemented |

### Serial transport

The default and supported backend is:

```text
native
```

The `libserialport` backend is present as a build option but is not currently
wired up.

---

## Multi-Device Operation

The GUI can discover and operate multiple MMS+ sensors.

Each device is tracked using its MMS+ device ID and serial port. Recording,
stopping, resetting, and syncing can be run per-device or across all detected
devices.

Output filenames include the device ID so recordings from multiple sensors can
share the same output directory without colliding.

---

## Development

Source builds, dependency installation, tests, Windows cross-compilation, Wine
smoke testing, and AppImage packaging are documented in:

[DEVELOPMENT.md](DEVELOPMENT.md)

---

## License

To be added.