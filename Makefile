# HeadMotion MMS client Makefile
#
# Common usage:
#   make debug
#   make release
#   make appimage
#
# Windows cross-compile from Linux/Arch:
#   make windows-cross-deps
#   make windows-cross-debug
#   make windows-cross-release
#
# Wine smoke tests:
#   make run-wine-gui
#   make run-wine-scan
#   make run-wine-identify WINE_PORT=COM1
#
# Native Windows builds still use the windows-debug/windows-release
# CMake presets directly from Windows.

GENERATOR ?= Ninja
SERIAL_BACKEND ?= native

CMAKE ?= cmake
CTEST ?= ctest
CPACK ?= cpack
WINE ?= wine

CONTAINER_ENGINE ?= docker

VCPKG ?= ./external/vcpkg/vcpkg
WINE_PORT ?= COM1

# ============================================================
# Build directories
# ============================================================

DEBUG_BUILD_DIR   := build/linux-native-debug
RELEASE_BUILD_DIR := build/linux-native-release

DEBUG_APP   := $(DEBUG_BUILD_DIR)/mmsctl
RELEASE_APP := $(RELEASE_BUILD_DIR)/mmsctl

DEBUG_GUI   := $(DEBUG_BUILD_DIR)/headmotion_gui
RELEASE_GUI := $(RELEASE_BUILD_DIR)/headmotion_gui

WINDOWS_CROSS_DEBUG_DIR   := build/windows-cross-debug
WINDOWS_CROSS_RELEASE_DIR := build/windows-cross-release

WINDOWS_CROSS_DEBUG_APP := $(WINDOWS_CROSS_DEBUG_DIR)/mmsctl.exe
WINDOWS_CROSS_DEBUG_GUI := $(WINDOWS_CROSS_DEBUG_DIR)/headmotion_gui.exe

WINDOWS_CROSS_RELEASE_APP := $(WINDOWS_CROSS_RELEASE_DIR)/mmsctl.exe
WINDOWS_CROSS_RELEASE_GUI := $(WINDOWS_CROSS_RELEASE_DIR)/headmotion_gui.exe

BOOKWORM_BUILDER_IMAGE := headmotion-bookworm-builder
BOOKWORM_BUILD_DIR := build/linux-bookworm-release

DIST_DIR := dist

# ============================================================
# Targets
# ============================================================

.PHONY: \
	all \
	debug release appimage \
	configure-debug configure-release \
	rebuild-debug rebuild-release \
	windows-cross-deps \
	configure-windows-cross-debug configure-windows-cross-release \
	windows-cross-debug windows-cross-release \
	rebuild-windows-cross-debug rebuild-windows-cross-release \
	clean-debug clean-release \
	clean-windows-cross-debug clean-windows-cross-release \
	clean-windows-cross clean distclean \
	test-debug test-release test \
	run-gui-debug run-gui-release \
	run-wine-gui run-wine-scan run-wine-identify \
	run-wine-record-start run-wine-record-stop \
	run-wine-sync run-wine-record-reset \
	run-scan run-identify \
	run-record-start run-record-stop \
	run-sync run-record-reset \
	help

all: debug

# ============================================================
# Linux Debug build
# ============================================================

configure-debug:
	$(CMAKE) -S . -B $(DEBUG_BUILD_DIR) -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DHEADMOTION_SERIAL_BACKEND=$(SERIAL_BACKEND) \
		-DHEADMOTION_BUILD_GUI=ON

debug: configure-debug
	$(CMAKE) --build $(DEBUG_BUILD_DIR)

rebuild-debug: clean-debug debug

clean-debug:
	@if [ -d "$(DEBUG_BUILD_DIR)" ]; then \
		$(CMAKE) --build $(DEBUG_BUILD_DIR) --target clean; \
	fi

# ============================================================
# Linux Release build
# ============================================================

configure-release:
	$(CMAKE) -S . -B $(RELEASE_BUILD_DIR) -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DHEADMOTION_SERIAL_BACKEND=$(SERIAL_BACKEND) \
		-DHEADMOTION_BUILD_GUI=ON

release: configure-release
	$(CMAKE) --build $(RELEASE_BUILD_DIR)

rebuild-release: clean-release release

clean-release:
	@if [ -d "$(RELEASE_BUILD_DIR)" ]; then \
		$(CMAKE) --build $(RELEASE_BUILD_DIR) --target clean; \
	fi

# ============================================================
# Windows cross-compile dependencies
#
# Requires Arch packages:
#   mingw-w64-gcc cmake ninja wine
#
# Assumes vcpkg is cloned at:
#   external/vcpkg
# ============================================================

windows-cross-deps:
	@command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 || \
		{ echo "Missing x86_64-w64-mingw32-gcc. Install mingw-w64-gcc."; exit 1; }
	@command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 || \
		{ echo "Missing x86_64-w64-mingw32-g++. Install mingw-w64-gcc."; exit 1; }
	@command -v ninja >/dev/null 2>&1 || \
		{ echo "Missing ninja."; exit 1; }
	@test -f external/vcpkg/bootstrap-vcpkg.sh || \
		{ echo "vcpkg not found at external/vcpkg"; \
		  echo "Clone it with: git clone https://github.com/microsoft/vcpkg.git external/vcpkg"; \
		  exit 1; }
	@if [ ! -x "$(VCPKG)" ]; then \
		./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics; \
	fi
	$(VCPKG) install fltk:x64-mingw-static

# ============================================================
# Windows cross-compile: Debug
# ============================================================

configure-windows-cross-debug:
	$(CMAKE) --preset windows-cross-debug

windows-cross-debug: configure-windows-cross-debug
	$(CMAKE) --build --preset windows-cross-debug

rebuild-windows-cross-debug: clean-windows-cross-debug windows-cross-debug

clean-windows-cross-debug:
	rm -rf $(WINDOWS_CROSS_DEBUG_DIR)

# ============================================================
# Windows cross-compile: Release
# ============================================================

configure-windows-cross-release:
	$(CMAKE) --preset windows-cross-release

windows-cross-release: configure-windows-cross-release
	$(CMAKE) --build --preset windows-cross-release

rebuild-windows-cross-release: clean-windows-cross-release windows-cross-release

clean-windows-cross-release:
	rm -rf $(WINDOWS_CROSS_RELEASE_DIR)

clean-windows-cross:
	rm -rf \
		$(WINDOWS_CROSS_DEBUG_DIR) \
		$(WINDOWS_CROSS_RELEASE_DIR)

# ============================================================
# Packaging
# ============================================================

# appimage: release
# 	rm -rf $(RELEASE_BUILD_DIR)/_CPack_Packages
# 	cd $(RELEASE_BUILD_DIR) && $(CPACK) -G AppImage
# 	@echo ""
# 	@echo "AppImage created:"
# 	@find $(RELEASE_BUILD_DIR) \
# 		-maxdepth 1 \
# 		-type f \
# 		-name '*.AppImage' \
# 		-print

# ============================================================
# Debian 12 AppImage
#
# Build the distributable AppImage inside Debian 12 so the
# resulting binary is compatible with Debian 12 and newer.
# ============================================================

appimage:
	$(CONTAINER_ENGINE) build \
		-f packaging/linux/Dockerfile.bookworm \
		-t $(BOOKWORM_BUILDER_IMAGE) \
		.

	$(CONTAINER_ENGINE) run --rm \
		--user "$$(id -u):$$(id -g)" \
		-e HOME=/tmp \
		-v "$$(pwd):/workspace" \
		-v "$$(pwd)/../MetaWear-SDK-Cpp:/workspace/external/MetaWear-SDK-Cpp:ro" \
		-w /workspace \
		$(BOOKWORM_BUILDER_IMAGE) \
		/bin/bash -c '\
			set -e; \
			rm -rf $(BOOKWORM_BUILD_DIR); \
			cmake \
				-S . \
				-B $(BOOKWORM_BUILD_DIR) \
				-G Ninja \
				-DCMAKE_BUILD_TYPE=Release \
				-DHEADMOTION_SERIAL_BACKEND=native \
				-DHEADMOTION_BUILD_GUI=ON; \
			cmake --build $(BOOKWORM_BUILD_DIR); \
			rm -rf $(BOOKWORM_BUILD_DIR)/_CPack_Packages; \
			cd $(BOOKWORM_BUILD_DIR); \
			cpack -G AppImage; \
		'

	mkdir -p $(DIST_DIR)
	cp $(BOOKWORM_BUILD_DIR)/*.AppImage $(DIST_DIR)/

	@echo ""
	@echo "Debian 12 compatible AppImage:"
	@find $(DIST_DIR) \
		-maxdepth 1 \
		-type f \
		-name '*.AppImage' \
		-print

# ============================================================
# Cleanup
# ============================================================

clean: clean-debug clean-release clean-windows-cross

distclean:
	rm -rf build

# ============================================================
# Tests
# ============================================================

test-debug: debug
	$(CTEST) \
		--test-dir $(DEBUG_BUILD_DIR) \
		--output-on-failure

test-release: release
	$(CTEST) \
		--test-dir $(RELEASE_BUILD_DIR) \
		--output-on-failure

test: test-debug

# ============================================================
# Linux GUI
# ============================================================

run-gui-debug: debug
	./$(DEBUG_GUI)

run-gui-release: release
	./$(RELEASE_GUI)

# ============================================================
# Wine smoke tests
#
# These execute the Windows Debug cross-build.
# WINE_PORT defaults to COM1.
# ============================================================

run-wine-gui: windows-cross-debug
	$(WINE) $(WINDOWS_CROSS_DEBUG_GUI)

run-wine-scan: windows-cross-debug
	$(WINE) $(WINDOWS_CROSS_DEBUG_APP) scan --json

run-wine-identify: windows-cross-debug
	$(WINE) $(WINDOWS_CROSS_DEBUG_APP) identify \
		--port $(WINE_PORT) \
		--json

run-wine-record-start: windows-cross-debug
	$(WINE) $(WINDOWS_CROSS_DEBUG_APP) record-start \
		--port $(WINE_PORT) \
		--rate $(or $(RATE),50) \
		--json

run-wine-record-stop: windows-cross-debug
	$(WINE) $(WINDOWS_CROSS_DEBUG_APP) record-stop \
		--port $(WINE_PORT) \
		--json

run-wine-sync: windows-cross-debug
	$(WINE) $(WINDOWS_CROSS_DEBUG_APP) sync \
		--port $(WINE_PORT) \
		--out $(or $(OUT),data/wine-test) \
		--json

run-wine-record-reset: windows-cross-debug
	$(WINE) $(WINDOWS_CROSS_DEBUG_APP) record-reset \
		--port $(WINE_PORT) \
		--json

# ============================================================
# Linux CLI development commands
#
# These use the Linux Debug build by default.
# ============================================================

run-scan: debug
	./$(DEBUG_APP) scan

run-identify: debug
ifndef PORT
	$(error PORT is required. Example: make run-identify PORT=/dev/ttyACM0)
endif
	./$(DEBUG_APP) identify --port $(PORT)

run-record-start: debug
ifndef PORT
	$(error PORT is required. Example: make run-record-start PORT=/dev/ttyACM0 RATE=50)
endif
	./$(DEBUG_APP) record-start \
		--port $(PORT) \
		--rate $(or $(RATE),50)

run-record-stop: debug
ifndef PORT
	$(error PORT is required. Example: make run-record-stop PORT=/dev/ttyACM0)
endif
	./$(DEBUG_APP) record-stop \
		--port $(PORT)

run-sync: debug
ifndef PORT
	$(error PORT is required. Example: make run-sync PORT=/dev/ttyACM0 OUT=data/sync)
endif
	./$(DEBUG_APP) sync \
		--port $(PORT) \
		--out $(or $(OUT),data/sync)

run-record-reset: debug
ifndef PORT
	$(error PORT is required. Example: make run-record-reset PORT=/dev/ttyACM0)
endif
	./$(DEBUG_APP) record-reset \
		--port $(PORT)

# ============================================================
# Help
# ============================================================

help:
	@echo "HeadMotion MMS client"
	@echo ""
	@echo "Linux builds:"
	@echo "  make debug"
	@echo "  make release"
	@echo "  make rebuild-debug"
	@echo "  make rebuild-release"
	@echo ""
	@echo "Windows cross-compile from Linux:"
	@echo "  make windows-cross-deps"
	@echo "      Verify MinGW/vcpkg and install fltk:x64-mingw-static"
	@echo ""
	@echo "  make windows-cross-debug"
	@echo "  make windows-cross-release"
	@echo "  make rebuild-windows-cross-debug"
	@echo "  make rebuild-windows-cross-release"
	@echo ""
	@echo "Wine:"
	@echo "  make run-wine-gui"
	@echo "  make run-wine-scan"
	@echo "  make run-wine-identify WINE_PORT=COM1"
	@echo "  make run-wine-record-start WINE_PORT=COM1 RATE=200"
	@echo "  make run-wine-record-stop WINE_PORT=COM1"
	@echo "  make run-wine-sync WINE_PORT=COM1 OUT=data/wine-test"
	@echo "  make run-wine-record-reset WINE_PORT=COM1"
	@echo ""
	@echo "Packaging:"
	@echo "  make appimage"
	@echo ""
	@echo "Cleanup:"
	@echo "  make clean-debug"
	@echo "  make clean-release"
	@echo "  make clean-windows-cross-debug"
	@echo "  make clean-windows-cross-release"
	@echo "  make clean-windows-cross"
	@echo "  make clean"
	@echo "  make distclean"
	@echo ""
	@echo "Linux GUI:"
	@echo "  make run-gui-debug"
	@echo "  make run-gui-release"
	@echo ""
	@echo "Tests:"
	@echo "  make test-debug"
	@echo "  make test-release"
	@echo ""
	@echo "Linux CLI development:"
	@echo "  make run-scan"
	@echo "  make run-identify PORT=/dev/ttyACM0"
	@echo "  make run-record-start PORT=/dev/ttyACM0 RATE=200"
	@echo "  make run-record-stop PORT=/dev/ttyACM0"
	@echo "  make run-sync PORT=/dev/ttyACM0 OUT=data/session_001"
	@echo "  make run-record-reset PORT=/dev/ttyACM0"
	@echo ""
	@echo "Variables:"
	@echo "  SERIAL_BACKEND=native|libserialport"
	@echo "  GENERATOR=Ninja"
	@echo "  WINE=wine"
	@echo "  WINE_PORT=COM1"
	@echo "  VCPKG=./external/vcpkg/vcpkg"