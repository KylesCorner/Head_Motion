#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

MMSCTL="$REPO_ROOT/build/linux-native-debug/mmsctl"
ESTIMATOR="$SCRIPT_DIR/estimate_mms_memory.py"
DEVICE="/dev/ttyACM0"
OUT_ROOT="$REPO_ROOT/data/mms_memory_trials"
DURATION=""
DURATION_SECONDS=""
GOAL_HOURS="8"
DO_SCAN=1

HZ_LIST=(25 50 100 200 400 800 1600 3200)

recording=0
current_hz=""

usage() {
    cat <<EOF
Usage:
  $0 --duration <duration> [options]

Required:
  --duration <duration>       How long to record each trial.
                              Examples: 10s, 5m, 8m36s, 1h, 1h30m

Options:
  --device <path>             Serial device. Default: $DEVICE
  --mmsctl <path>             Path to mmsctl. Default: $MMSCTL
  --estimator <path>          Path to estimate_mms_memory.py. Default: $ESTIMATOR
  --out <dir>                 Output root directory. Default: $OUT_ROOT
  --goal-hours <hours>        Goal duration passed to estimator. Default: $GOAL_HOURS
  --hz-list "25 50 100"       Override Hz list.
  --skip-scan                 Skip mmsctl scan before each trial.
  -h, --help                  Show this help.

Example:
  $0 --duration 10m --device /dev/ttyACM0

Default Hz list:
  ${HZ_LIST[*]}

Output naming:
  CSV:            session_<hz>hz.csv
  estimate text:  session_<hz>hz_estimate.txt
  workflow log:   logs/session_<hz>hz_workflow.log
EOF
}

log() {
    printf '[%s] %s\n' "$(date -Is)" "$*"
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

iso_from_epoch() {
    local epoch="$1"

    if date -d "@$epoch" -Is >/dev/null 2>&1; then
        date -d "@$epoch" -Is
    elif date -r "$epoch" -Is >/dev/null 2>&1; then
        date -r "$epoch" -Is
    else
        echo "$epoch"
    fi
}

duration_to_seconds() {
    local input="$1"
    local s="${input//[[:space:]]/}"
    local total=0

    if [[ "$s" =~ ^[0-9]+$ ]]; then
        echo "$s"
        return 0
    fi

    while [[ -n "$s" ]]; do
        if [[ "$s" =~ ^([0-9]+)(h|hr|hrs|hour|hours)(.*)$ ]]; then
            total=$((total + BASH_REMATCH[1] * 3600))
            s="${BASH_REMATCH[3]}"
        elif [[ "$s" =~ ^([0-9]+)(m|min|mins|minute|minutes)(.*)$ ]]; then
            total=$((total + BASH_REMATCH[1] * 60))
            s="${BASH_REMATCH[3]}"
        elif [[ "$s" =~ ^([0-9]+)(s|sec|secs|second|seconds)(.*)$ ]]; then
            total=$((total + BASH_REMATCH[1]))
            s="${BASH_REMATCH[3]}"
        else
            return 1
        fi
    done

    if [[ "$total" -le 0 ]]; then
        return 1
    fi

    echo "$total"
}

stop_recording_safely() {
    if [[ "$recording" -eq 1 ]]; then
        log "Attempting emergency record-stop for ${current_hz} Hz on ${DEVICE}"
        "$MMSCTL" record-stop "$DEVICE" || true
        recording=0
    fi
}

on_exit() {
    stop_recording_safely
}

trap on_exit EXIT INT TERM

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --duration)
                DURATION="${2:-}"
                shift 2
                ;;
            --device)
                DEVICE="${2:-}"
                shift 2
                ;;
            --mmsctl)
                MMSCTL="${2:-}"
                shift 2
                ;;
            --estimator)
                ESTIMATOR="${2:-}"
                shift 2
                ;;
            --out)
                OUT_ROOT="${2:-}"
                shift 2
                ;;
            --goal-hours)
                GOAL_HOURS="${2:-}"
                shift 2
                ;;
            --hz-list)
                read -r -a HZ_LIST <<< "${2:-}"
                shift 2
                ;;
            --skip-scan)
                DO_SCAN=0
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "Unknown argument: $1"
                ;;
        esac
    done
}

validate() {
    [[ -n "$DURATION" ]] || die "--duration is required"

    if ! DURATION_SECONDS="$(duration_to_seconds "$DURATION")"; then
        die "Invalid duration: $DURATION. Use forms like 10s, 5m, 8m36s, 1h, 1h30m."
    fi

    [[ -x "$MMSCTL" ]] || die "mmsctl not found or not executable: $MMSCTL"
    [[ -f "$ESTIMATOR" ]] || die "Estimator script not found: $ESTIMATOR"
    [[ -e "$DEVICE" ]] || die "Device does not exist: $DEVICE"

    command -v python3 >/dev/null 2>&1 || die "python3 not found"
    command -v awk >/dev/null 2>&1 || die "awk not found"
    command -v sleep >/dev/null 2>&1 || die "sleep not found"

    [[ "${#HZ_LIST[@]}" -gt 0 ]] || die "Hz list is empty"

    mkdir -p "$OUT_ROOT"
    mkdir -p "$OUT_ROOT/logs"
}

run_trial() {
    local hz="$1"
    current_hz="$hz"

    local trial_name="session_${hz}hz"
    local csv_out="$OUT_ROOT/${trial_name}.csv"
    local estimate_out="$OUT_ROOT/${trial_name}_estimate.txt"
    local workflow_log="$OUT_ROOT/logs/${trial_name}_workflow.log"

    rm -f "$csv_out" "$estimate_out" "$workflow_log"

    log "============================================================"
    log "Starting trial: ${hz} Hz"
    log "Final CSV:     $csv_out"
    log "Estimate text: $estimate_out"
    log "Workflow log:  $workflow_log"

    local workflow_start_s
    local workflow_end_s
    local workflow_elapsed_s
    local sync_start_s
    local sync_end_s
    local sync_elapsed_s

    workflow_start_s="$(date +%s)"

    {
        echo "Trial:                       $trial_name"
        echo "Rate:                        ${hz} Hz"
        echo "Device:                      $DEVICE"
        echo "Requested record duration:   $DURATION"
        echo "Requested duration seconds:  $DURATION_SECONDS"
        echo "Started:                     $(date -Is)"
        echo
    } > "$workflow_log"

    if [[ "$DO_SCAN" -eq 1 ]]; then
        log "Scanning..."
        {
            echo "----- mmsctl scan -----"
            "$MMSCTL" scan
            echo
        } >> "$workflow_log" 2>&1
    fi

    log "Resetting logger..."
    {
        echo "----- mmsctl record-reset -----"
        "$MMSCTL" record-reset "$DEVICE"
        echo
    } >> "$workflow_log" 2>&1

    log "Starting logger at ${hz} Hz..."
    {
        echo "----- mmsctl record-start --rate ${hz} -----"
        "$MMSCTL" record-start "$DEVICE" --rate "$hz"
        echo
    } >> "$workflow_log" 2>&1

    recording=1

    log "Recording for ${DURATION} (${DURATION_SECONDS}s)..."
    sleep "$DURATION_SECONDS"

    log "Stopping logger..."
    {
        echo "----- mmsctl record-stop -----"
        "$MMSCTL" record-stop "$DEVICE"
        echo
    } >> "$workflow_log" 2>&1

    recording=0

    log "Syncing/download directly to CSV..."
    sync_start_s="$(date +%s)"

    {
        echo "----- mmsctl sync -----"
        "$MMSCTL" sync "$DEVICE" --out "$csv_out"
        echo
    } >> "$workflow_log" 2>&1

    sync_end_s="$(date +%s)"
    sync_elapsed_s="$((sync_end_s - sync_start_s))"

    workflow_end_s="$(date +%s)"
    workflow_elapsed_s="$((workflow_end_s - workflow_start_s))"

    if [[ ! -f "$csv_out" ]]; then
        {
            echo
            echo "ERROR: Expected CSV was not created: $csv_out"
            echo "Workflow elapsed seconds: $workflow_elapsed_s"
            echo "Sync elapsed seconds:     $sync_elapsed_s"
        } >> "$workflow_log"

        die "CSV was not created for ${trial_name}. Check $workflow_log"
    fi

    if [[ ! -s "$csv_out" ]]; then
        {
            echo
            echo "ERROR: CSV was created but is empty: $csv_out"
            echo "Workflow elapsed seconds: $workflow_elapsed_s"
            echo "Sync elapsed seconds:     $sync_elapsed_s"
        } >> "$workflow_log"

        die "CSV is empty for ${trial_name}. Check $workflow_log"
    fi

    log "Running estimator..."

    {
        echo "MetaMotion S Memory Trial Estimate"
        echo "=================================="
        echo "Trial:                     $trial_name"
        echo "Rate:                      ${hz} Hz"
        echo "Requested record duration: $DURATION"
        echo "Requested duration seconds:$DURATION_SECONDS"
        echo "Workflow start:            $(iso_from_epoch "$workflow_start_s")"
        echo "Workflow end:              $(iso_from_epoch "$workflow_end_s")"
        echo "Workflow elapsed seconds:  $workflow_elapsed_s"
        echo "Workflow elapsed minutes:  $(awk "BEGIN { printf \"%.3f\", $workflow_elapsed_s / 60.0 }")"
        echo "Sync elapsed seconds:      $sync_elapsed_s"
        echo "Sync elapsed minutes:      $(awk "BEGIN { printf \"%.3f\", $sync_elapsed_s / 60.0 }")"
        echo "Final CSV:                 $csv_out"
        echo
        echo "Estimator output"
        echo "================"
        python3 "$ESTIMATOR" "$csv_out" --hz "$hz" --goal-hours "$GOAL_HOURS"
    } > "$estimate_out" 2>&1

    {
        echo
        echo "Completed:                 $(date -Is)"
        echo "Workflow elapsed seconds:  $workflow_elapsed_s"
        echo "Workflow elapsed minutes:  $(awk "BEGIN { printf \"%.3f\", $workflow_elapsed_s / 60.0 }")"
        echo "Sync elapsed seconds:      $sync_elapsed_s"
        echo "Sync elapsed minutes:      $(awk "BEGIN { printf \"%.3f\", $sync_elapsed_s / 60.0 }")"
        echo "Final CSV:                 $csv_out"
        echo "Estimate text:             $estimate_out"
    } >> "$workflow_log"

    log "Completed ${hz} Hz trial"
    log "Workflow elapsed: ${workflow_elapsed_s}s"
    log "Download/sync elapsed: ${sync_elapsed_s}s"
}

main() {
    parse_args "$@"
    validate

    log "Starting MMS memory experiment series"
    log "Hz list: ${HZ_LIST[*]}"
    log "Duration per trial: $DURATION (${DURATION_SECONDS}s)"
    log "Output root: $OUT_ROOT"
    log "mmsctl: $MMSCTL"
    log "estimator: $ESTIMATOR"

    for hz in "${HZ_LIST[@]}"; do
        run_trial "$hz"
    done

    log "All trials complete"
    log "Results are in: $OUT_ROOT"
}

main "$@"
