# HeadMotion MMS client Makefile
#
# Common usage:
#   make debug
#   make release
#   make appimage
#
#   make rebuild-debug
#   make rebuild-release
#
#   make run-gui-debug
#   make run-gui-release
#
#   make run-scan
#   make run-identify PORT=/dev/ttyACM0
#
#   make clean
#   make distclean

GENERATOR ?= Ninja
SERIAL_BACKEND ?= native

CMAKE ?= cmake
CTEST ?= ctest
CPACK ?= cpack

# ============================================================
# Build directories
# ============================================================

DEBUG_BUILD_DIR   := build/linux-native-debug
RELEASE_BUILD_DIR := build/linux-native-release

DEBUG_APP   := $(DEBUG_BUILD_DIR)/mmsctl
RELEASE_APP := $(RELEASE_BUILD_DIR)/mmsctl

DEBUG_GUI   := $(DEBUG_BUILD_DIR)/headmotion_gui
RELEASE_GUI := $(RELEASE_BUILD_DIR)/headmotion_gui

# ============================================================
# Targets
# ============================================================

.PHONY: \
	all \
	debug release appimage \
	configure-debug configure-release \
	rebuild-debug rebuild-release \
	clean-debug clean-release clean distclean \
	test-debug test-release test \
	run-gui-debug run-gui-release \
	run-scan run-identify \
	run-record-start run-record-stop \
	run-sync run-record-reset \
	help

all: debug

# ============================================================
# Debug build
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
# Release build
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
# Packaging
# ============================================================

appimage: release
	rm -rf $(RELEASE_BUILD_DIR)/_CPack_Packages
	cd $(RELEASE_BUILD_DIR) && $(CPACK) -G AppImage
	@echo ""
	@echo "AppImage created:"
	@find $(RELEASE_BUILD_DIR) \
		-maxdepth 1 \
		-type f \
		-name '*.AppImage' \
		-print

# ============================================================
# Cleanup
# ============================================================

clean: clean-debug clean-release

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
# GUI
# ============================================================

run-gui-debug: debug
	./$(DEBUG_GUI)

run-gui-release: release
	./$(RELEASE_GUI)

# ============================================================
# CLI development commands
#
# These use the Debug build by default.
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
	@echo "Build targets:"
	@echo "  make debug"
	@echo "      Build Debug configuration"
	@echo ""
	@echo "  make release"
	@echo "      Build Release configuration"
	@echo ""
	@echo "  make rebuild-debug"
	@echo "  make rebuild-release"
	@echo ""
	@echo "Packaging:"
	@echo "  make appimage"
	@echo "      Build Release and create the Linux AppImage"
	@echo ""
	@echo "Cleanup:"
	@echo "  make clean-debug"
	@echo "  make clean-release"
	@echo "  make clean"
	@echo "  make distclean"
	@echo ""
	@echo "GUI:"
	@echo "  make run-gui-debug"
	@echo "  make run-gui-release"
	@echo ""
	@echo "Tests:"
	@echo "  make test-debug"
	@echo "  make test-release"
	@echo ""
	@echo "CLI development:"
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