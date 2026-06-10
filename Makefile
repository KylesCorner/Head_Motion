# HeadMotion MMS client Makefile
#
# Common usage:
#   make configure
#   make build
#   make run-scan
#   make run-identify PORT=/dev/ttyACM0
#   make clean

BUILD_TYPE ?= Debug
GENERATOR ?= Ninja
SERIAL_BACKEND ?= native
BUILD_DIR ?= build/linux-native-debug

CMAKE ?= cmake
CTEST ?= ctest

APP := $(BUILD_DIR)/mmsctl

.PHONY: all configure build rebuild clean distclean test run-scan run-identify help

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DHEADMOTION_SERIAL_BACKEND=$(SERIAL_BACKEND)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

rebuild: clean build

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean

distclean:
	rm -rf build

test: build
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

run-scan: build
	./$(APP) scan

run-identify: build
ifndef PORT
	$(error PORT is required. Example: make run-identify PORT=/dev/ttyACM0)
endif
	./$(APP) identify $(PORT)

run-record-start: build
ifndef PORT
	$(error PORT is required. Example: make run-record-start PORT=/dev/ttyACM0 RATE=50)
endif
ifndef RATE
	$(eval RATE := 50)
endif
	./$(APP) record-start $(PORT) --rate $(RATE)

run-record-stop: build
ifndef PORT
	$(error PORT is required. Example: make run-record-stop PORT=/dev/ttyACM0)
endif
	./$(APP) record-stop $(PORT)


run-sync: build
ifndef PORT
	$(error PORT is required. Example: make run-sync PORT=/dev/ttyACM0 OUT=data/sync.csv)
endif
ifndef OUT
	$(eval OUT := data/sync.csv)
endif
	./$(APP) sync $(PORT) --out $(OUT)

run-record-reset: build
ifndef PORT
	$(error PORT is required. Example: make run-record-reset PORT=/dev/ttyACM0)
endif
	./$(APP) record-reset $(PORT)


help:
	@echo "HeadMotion MMS client"
	@echo ""
	@echo "Targets:"
	@echo "  make configure"
	@echo "  make build"
	@echo "  make rebuild"
	@echo "  make clean"
	@echo "  make distclean"
	@echo "  make test"
	@echo "  make run-scan"
	@echo "  make run-identify PORT=/dev/ttyACM0"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE=Debug|Release"
	@echo "  SERIAL_BACKEND=native|libserialport"
	@echo "  BUILD_DIR=build/linux-native-debug"
	@echo "  GENERATOR=Ninja"