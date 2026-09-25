# HeadMotion Development Setup

This document covers source builds, development dependencies, testing,
cross-compilation, and packaging for HeadMotion.

For normal installation and usage, see [README.md](README.md).

---

## Requirements

HeadMotion is a C++20 project built with CMake.

The source tree expects the MbientLab MetaWear C++ SDK at:

```text
external/MetaWear-SDK-Cpp
```

The primary build outputs are:

```text
mmsctl
headmotion_gui
```

---

## Repository Setup

Clone HeadMotion:

```bash
git clone https://github.com/KylesCorner/Head_Motion.git
cd Head_Motion
```

Clone the MetaWear C++ SDK:

```bash
mkdir -p external

git clone https://github.com/mbientlab/MetaWear-SDK-Cpp.git \
    external/MetaWear-SDK-Cpp
```

Initialize the SDK submodules:

```bash
git -C external/MetaWear-SDK-Cpp \
    submodule update --init --recursive
```

The relevant layout should be:

```text
Head_Motion/
├── external/
│   └── MetaWear-SDK-Cpp/
├── include/
├── packaging/
├── src/
├── tests/
├── CMakeLists.txt
├── CMakePresets.json
└── Makefile
```

---

## Linux Development Build

### Arch Linux

Install dependencies:

```bash
sudo pacman -S \
    base-devel \
    cmake \
    ninja \
    git \
    fltk
```

### Debian / Ubuntu

Install dependencies:

```bash
sudo apt update

sudo apt install \
    build-essential \
    cmake \
    ninja-build \
    git \
    libfltk1.3-dev
```

Depending on the FLTK package version, `fluid` may also be required.

### Debug build

The easiest development build is:

```bash
make debug
```

This configures:

```text
HEADMOTION_SERIAL_BACKEND=native
HEADMOTION_BUILD_GUI=ON
HEADMOTION_BUILD_TESTS=ON
```

Build directory:

```text
build/linux-native-debug/
```

Executables:

```text
build/linux-native-debug/mmsctl
build/linux-native-debug/headmotion_gui
```

Run the GUI:

```bash
make run-gui-debug
```

or directly:

```bash
./build/linux-native-debug/headmotion_gui
```

### Release build

```bash
make release
```

Build directory:

```text
build/linux-native-release/
```

Executables:

```text
build/linux-native-release/mmsctl
build/linux-native-release/headmotion_gui
```

Run the Release GUI:

```bash
make run-gui-release
```

---

## Running Tests

Build and run the Debug test suite:

```bash
make test-debug
```

Release tests:

```bash
make test-release
```

Run CTest directly:

```bash
ctest \
    --test-dir build/linux-native-debug \
    --output-on-failure
```

To rebuild Debug from a clean target state:

```bash
make rebuild-debug
```

To completely remove generated build directories:

```bash
make distclean
```

---

## Development CLI Shortcuts

The Makefile provides wrappers around common Debug CLI operations.

Scan:

```bash
make run-scan
```

Identify:

```bash
make run-identify PORT=/dev/ttyACM0
```

Reset:

```bash
make run-record-reset PORT=/dev/ttyACM0
```

Start recording:

```bash
make run-record-start \
    PORT=/dev/ttyACM0 \
    RATE=200
```

Stop:

```bash
make run-record-stop PORT=/dev/ttyACM0
```

Sync:

```bash
make run-sync \
    PORT=/dev/ttyACM0 \
    OUT=data/session_001
```

For flags not exposed through the Makefile wrapper, invoke `mmsctl` directly.

Example legacy-CSV sync:

```bash
./build/linux-native-debug/mmsctl sync \
    --port /dev/ttyACM0 \
    --out data/session_001 \
    --imu-csv
```

---

## Common Make Targets

| Command | Description |
| --- | --- |
| `make debug` | Configure and build Linux Debug |
| `make release` | Configure and build Linux Release |
| `make rebuild-debug` | Clean and rebuild Linux Debug |
| `make rebuild-release` | Clean and rebuild Linux Release |
| `make run-gui-debug` | Build and run the Debug GUI |
| `make run-gui-release` | Build and run the Release GUI |
| `make test-debug` | Build and run Debug tests |
| `make test-release` | Build and run Release tests |
| `make windows-cross-deps` | Verify/install MinGW cross-build dependencies |
| `make windows-cross-debug` | Cross-build Windows Debug |
| `make windows-cross-release` | Cross-build Windows Release |
| `make run-wine-gui` | Run the Windows Debug GUI under Wine |
| `make run-wine-scan` | Run Windows `mmsctl scan` under Wine |
| `make appimage` | Build the Linux AppImage |
| `make clean` | Clean Linux and Windows cross builds |
| `make distclean` | Remove the entire `build/` directory |
| `make help` | Print Makefile targets |

---

## Native Windows Build

HeadMotion has a native Windows serial backend.

The current CMake presets are:

```text
windows-debug
windows-release
```

The presets use:

```text
Visual Studio 18 2026
x64
vcpkg
```

### 1. Install prerequisites

Install:

- Git
- CMake
- Visual Studio with the C++ desktop development workload

Clone vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git external/vcpkg
```

Bootstrap it:

```powershell
.\external\vcpkg\bootstrap-vcpkg.bat
```

Install FLTK:

```powershell
.\external\vcpkg\vcpkg.exe install fltk:x64-windows
```

The MetaWear SDK must also exist at:

```text
external/MetaWear-SDK-Cpp
```

### 2. Configure and build Debug

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
```

Output directory:

```text
build/windows-native-debug/
```

### 3. Configure and build Release

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
```

Output directory:

```text
build/windows-native-release/
```

If the installed Visual Studio version differs from the generator currently
specified in `CMakePresets.json`, update the preset or select an appropriate
generator for that development machine.

---

## Windows Cross-Compilation from Linux

The project can also cross-compile Windows x64 executables from Linux with
MinGW-w64.

The current Makefile workflow is primarily set up for Arch Linux.

Install host dependencies:

```bash
sudo pacman -S \
    mingw-w64-gcc \
    cmake \
    ninja \
    wine \
    git
```

Clone vcpkg if it is not already present:

```bash
git clone https://github.com/microsoft/vcpkg.git external/vcpkg
```

Prepare the Windows cross dependencies:

```bash
make windows-cross-deps
```

This installs:

```text
fltk:x64-mingw-static
```

### Debug cross-build

```bash
make windows-cross-debug
```

Outputs:

```text
build/windows-cross-debug/mmsctl.exe
build/windows-cross-debug/headmotion_gui.exe
```

### Release cross-build

```bash
make windows-cross-release
```

Outputs:

```text
build/windows-cross-release/mmsctl.exe
build/windows-cross-release/headmotion_gui.exe
```

The MinGW runtime is linked statically when
`HEADMOTION_STATIC_MINGW_RUNTIME=ON`.

---

## Wine Smoke Tests

The Makefile provides basic Wine tests for Windows cross-builds.

GUI:

```bash
make run-wine-gui
```

Scan:

```bash
make run-wine-scan
```

Identify:

```bash
make run-wine-identify WINE_PORT=COM1
```

Start recording:

```bash
make run-wine-record-start \
    WINE_PORT=COM1 \
    RATE=200
```

Stop:

```bash
make run-wine-record-stop WINE_PORT=COM1
```

Sync:

```bash
make run-wine-sync \
    WINE_PORT=COM1 \
    OUT=data/wine-test
```

Reset:

```bash
make run-wine-record-reset WINE_PORT=COM1
```

These tests verify executable behavior under Wine; actual MMS+ access also
depends on the serial-device mapping available to the Wine environment.

---

## AppImage Packaging

The Linux distributable is built in a Debian 12 container to provide a
relatively conservative runtime base.

Requirements:

- Docker or a compatible container engine
- MetaWear SDK source
- `appimagetool` is installed inside the builder image

Build:

```bash
make appimage
```

The Makefile builds the container image, performs a Release build inside the
container, runs CPack's AppImage generator, and copies the resulting artifact
to:

```text
dist/
```

### Current SDK bind-mount requirement

The current `make appimage` target bind-mounts:

```text
../MetaWear-SDK-Cpp
```

into:

```text
/workspace/external/MetaWear-SDK-Cpp
```

Therefore, for the current packaging target, place or clone a MetaWear SDK
checkout beside the HeadMotion repository:

```text
parent/
├── Head_Motion/
└── MetaWear-SDK-Cpp/
```

This is separate from the normal development layout, which expects:

```text
Head_Motion/external/MetaWear-SDK-Cpp/
```

---

## CMake Configuration

Important project options:

```text
HEADMOTION_BUILD_TESTS
HEADMOTION_BUILD_GUI
HEADMOTION_STATIC_MINGW_RUNTIME
HEADMOTION_SERIAL_BACKEND
METAWEAR_SDK_DIR
```

Default serial backend:

```text
native
```

Supported native implementations:

```text
Linux
Windows
```

The following backend is not currently wired up:

```text
libserialport
```

The native macOS serial backend is also not implemented.

---

## Project Structure

```text
src/
├── app/            Application commands
├── gui/            FLTK GUI
├── metawear/       MetaWear-over-USB transport
├── platform/       Native Linux/Windows serial implementations
├── protocol/       MMS+ USB framing
├── sdk/            MetaWear SDK bridge
├── session/        Device/session state
└── util/           Portable utilities
```

Public headers are under:

```text
include/headmotion/
```

Tests are under:

```text
tests/
```

---

## Build Troubleshooting

### MetaWear SDK not found

If CMake reports that the MetaWear SDK is missing, verify:

```bash
test -f external/MetaWear-SDK-Cpp/CMakeLists.txt
```

If it is absent:

```bash
git clone https://github.com/mbientlab/MetaWear-SDK-Cpp.git \
    external/MetaWear-SDK-Cpp

git -C external/MetaWear-SDK-Cpp \
    submodule update --init --recursive
```

### FLTK not found

Verify that the development package for FLTK is installed.

Arch:

```bash
sudo pacman -S fltk
```

Debian/Ubuntu:

```bash
sudo apt install libfltk1.3-dev
```

Windows/vcpkg:

```powershell
.\external\vcpkg\vcpkg.exe install fltk:x64-windows
```

### Linux serial permission denied

Check:

```bash
ls -l /dev/ttyACM*
```

Arch:

```bash
sudo usermod -aG uucp "$USER"
```

Debian/Ubuntu:

```bash
sudo usermod -aG dialout "$USER"
```

Log out and back in after adding the group.

---

## Release Checklist

Before creating a release:

1. Build Debug.
2. Run the full test suite.
3. Build Release.
4. Test the GUI with physical MMS+ hardware.
5. Test `record-reset`, `record-start`, `record-stop`, and `sync`.
6. Verify both default and legacy CSV output.
7. Verify multi-device discovery if hardware is available.
8. Build the Linux AppImage.
9. Build/test the Windows release artifact.
10. Confirm the project version in `CMakeLists.txt`.