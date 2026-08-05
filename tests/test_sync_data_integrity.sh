#!/usr/bin/env bash
#
# Hardware integration test for mmsctl sync CSV data integrity.
#
# Verifies:
#   1. Existing CSV files are never overwritten.
#   2. The next unused numbered filename is selected.
#   3. IMU CSV output contains exactly one header and at least one data row.
#   4. CSV files are opened with O_APPEND and without O_TRUNC.
#
# Environment overrides:
#   BIN=./build/linux-native-debug/mmsctl
#   PORT=/dev/ttyACM0
#   RATE=50
#   RECORD_SECONDS=10
#   OUT_ROOT=data
#
# Example:
#   RECORD_SECONDS=20 ./tools/test_sync_data_integrity.sh
#

set -Eeuo pipefail

BIN="${BIN:-./build/linux-native-debug/mmsctl}"
PORT="${PORT:-/dev/ttyACM0}"
RATE="${RATE:-50}"
RECORD_SECONDS="${RECORD_SECONDS:-10}"
OUT_ROOT="${OUT_ROOT:-data}"

RECORDING=0
TEST_DIR=""
TRACE_FILE=""

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

pass() {
    printf 'PASS: %s\n' "$*"
}

cleanup() {
    local exit_code=$?

    if [[ "$RECORDING" -eq 1 ]]; then
        printf '\nStopping recording after interrupted/failed test...\n' >&2
        "$BIN" record-stop "$PORT" >/dev/null 2>&1 || true
    fi

    if [[ "$exit_code" -ne 0 && -n "$TEST_DIR" ]]; then
        printf 'Test artifacts preserved in: %s\n' "$TEST_DIR" >&2
        [[ -n "$TRACE_FILE" ]] &&
            printf 'strace log: %s\n' "$TRACE_FILE" >&2
    fi

    exit "$exit_code"
}

trap cleanup EXIT INT TERM

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        fail "Required command not found: $1"
}

sha256_of() {
    sha256sum "$1" | awk '{print $1}'
}

assert_file_unchanged() {
    local path=$1
    local expected_hash=$2
    local actual_hash

    [[ -f "$path" ]] || fail "Existing file disappeared: $path"

    actual_hash="$(sha256_of "$path")"
    [[ "$actual_hash" == "$expected_hash" ]] ||
        fail "Existing file was modified: $path"
}

find_open_trace_line() {
    local path=$1

    grep -F "\"$path\"" "$TRACE_FILE" |
        grep -E 'openat\(' |
        tail -n 1 || true
}

assert_opened_append_only() {
    local path=$1
    local trace_line

    trace_line="$(find_open_trace_line "$path")"

    [[ -n "$trace_line" ]] ||
        fail "Could not find openat() call for $path in $TRACE_FILE"

    [[ "$trace_line" == *"O_APPEND"* ]] ||
        fail "File was not opened with O_APPEND: $trace_line"

    [[ "$trace_line" != *"O_TRUNC"* ]] ||
        fail "File was opened with destructive O_TRUNC: $trace_line"

    pass "$(basename "$path") opened with O_APPEND and without O_TRUNC"
}

require_command realpath
require_command sha256sum
require_command awk
require_command grep
require_command strace

[[ -x "$BIN" ]] || fail "mmsctl is not executable: $BIN"
[[ -e "$PORT" ]] || fail "Serial device does not exist: $PORT"
[[ "$RATE" =~ ^[0-9]+$ ]] || fail "RATE must be a positive integer"
[[ "$RECORD_SECONDS" =~ ^[0-9]+$ ]] ||
    fail "RECORD_SECONDS must be a positive integer"
(( RATE > 0 )) || fail "RATE must be greater than zero"
(( RECORD_SECONDS > 0 )) ||
    fail "RECORD_SECONDS must be greater than zero"

mkdir -p "$OUT_ROOT"
OUT_ROOT="$(realpath "$OUT_ROOT")"
TEST_DIR="$OUT_ROOT/sync_integrity_test_$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "$TEST_DIR"
TRACE_FILE="$TEST_DIR/sync.strace"

printf 'Binary:           %s\n' "$BIN"
printf 'Serial port:      %s\n' "$PORT"
printf 'Sample rate:      %s Hz\n' "$RATE"
printf 'Record duration:  %s seconds\n' "$RECORD_SECONDS"
printf 'Test directory:   %s\n\n' "$TEST_DIR"

printf 'Scanning for connected devices...\n'
"$BIN" scan

printf '\nResetting previous onboard recording state...\n'
"$BIN" record-reset "$PORT"
sleep 3

printf '\nStarting a fresh recording...\n'
"$BIN" record-start "$PORT" --rate "$RATE"
RECORDING=1

printf '\nRecording for %s seconds.\n' "$RECORD_SECONDS"
printf 'Move or wear the sensor now to produce recognizable IMU data.\n'
sleep "$RECORD_SECONDS"

printf '\nStopping recording...\n'
"$BIN" record-stop "$PORT"
RECORDING=0

printf '\nCreating protected sentinel files...\n'

cat >"$TEST_DIR/imu.csv" <<'EOF'
DO NOT OVERWRITE imu.csv
EOF

cat >"$TEST_DIR/imu_1.csv" <<'EOF'
DO NOT OVERWRITE imu_1.csv
EOF

cat >"$TEST_DIR/battery.csv" <<'EOF'
DO NOT OVERWRITE battery.csv
EOF

cat >"$TEST_DIR/battery_1.csv" <<'EOF'
DO NOT OVERWRITE battery_1.csv
EOF

declare -A SENTINEL_HASHES=()

for sentinel in \
    "$TEST_DIR/imu.csv" \
    "$TEST_DIR/imu_1.csv" \
    "$TEST_DIR/battery.csv" \
    "$TEST_DIR/battery_1.csv"
do
    SENTINEL_HASHES["$sentinel"]="$(sha256_of "$sentinel")"
done

printf 'Running sync under strace...\n'
strace \
    -f \
    -s 4096 \
    -e trace=openat \
    -o "$TRACE_FILE" \
    "$BIN" sync "$PORT" --out "$TEST_DIR"

printf '\nVerifying existing files were untouched...\n'
for sentinel in "${!SENTINEL_HASHES[@]}"; do
    assert_file_unchanged "$sentinel" "${SENTINEL_HASHES[$sentinel]}"
    pass "Preserved $(basename "$sentinel")"
done

IMU_OUTPUT="$TEST_DIR/imu_2.csv"
BATTERY_OUTPUT="$TEST_DIR/battery_2.csv"

[[ -f "$IMU_OUTPUT" ]] ||
    fail "Expected collision-free output was not created: $IMU_OUTPUT"

pass "Selected next unused IMU filename: $(basename "$IMU_OUTPUT")"

expected_imu_header='epoch_ms,sensor,x,y,z'
actual_imu_header="$(head -n 1 "$IMU_OUTPUT")"

[[ "$actual_imu_header" == "$expected_imu_header" ]] ||
    fail "Unexpected IMU header: '$actual_imu_header'"

imu_header_count="$(
    grep -c '^epoch_ms,sensor,x,y,z$' "$IMU_OUTPUT" || true
)"

[[ "$imu_header_count" -eq 1 ]] ||
    fail "Expected exactly one IMU header, found $imu_header_count"

imu_line_count="$(wc -l <"$IMU_OUTPUT")"
(( imu_line_count > 1 )) ||
    fail "IMU output contains no data rows: $IMU_OUTPUT"

pass "IMU CSV has one header and $((imu_line_count - 1)) data rows"

assert_opened_append_only "$IMU_OUTPUT"

if [[ -f "$BATTERY_OUTPUT" ]]; then
    expected_battery_header='epoch_ms,voltage_mv,charge_percent'
    actual_battery_header="$(head -n 1 "$BATTERY_OUTPUT")"

    [[ "$actual_battery_header" == "$expected_battery_header" ]] ||
        fail "Unexpected battery header: '$actual_battery_header'"

    battery_header_count="$(
        grep -c '^epoch_ms,voltage_mv,charge_percent$' \
            "$BATTERY_OUTPUT" || true
    )"

    [[ "$battery_header_count" -eq 1 ]] ||
        fail "Expected exactly one battery header, found $battery_header_count"

    assert_opened_append_only "$BATTERY_OUTPUT"
    pass "Battery CSV created and validated"
else
    printf 'INFO: Battery logging is disabled; no battery_2.csv was expected.\n'
fi

printf '\nAll sync data-integrity checks passed.\n'
printf 'Test artifacts: %s\n' "$TEST_DIR"
