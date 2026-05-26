BUILD_DIR := build
BUILD_TYPE ?= Debug

.PHONY: all configure build run test clean rebuild

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j"$$(nproc)"

run: build
	./$(BUILD_DIR)/headmotion

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build
