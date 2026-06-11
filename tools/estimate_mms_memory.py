#!/usr/bin/env python3

import argparse
import csv
import math
import os
import sys
from collections import defaultdict
from dataclasses import dataclass

# MMS logger capacity model
#
# MbientLab-style logger math:
#   - 1 log entry = 8 bytes
#   - MMS capacity ≈ 67,108,864 log entries
#   - raw accel XYZ sample = 2 log entries
#   - raw gyro XYZ sample  = 2 log entries
#
# Your CSV rows:
#   epoch_ms,sensor,x,y,z
#
# Each row is one XYZ sample from either accel_g or gyro_dps.
MMS_CAPACITY_ENTRIES = 67_108_864
LOG_ENTRY_BYTES = 8

ENTRIES_PER_SAMPLE = {
    "accel_g": 2,
    "gyro_dps": 2,
}


@dataclass
class SensorStats:
    count: int = 0
    first_epoch_ms: int | None = None
    last_epoch_ms: int | None = None
    min_epoch_ms: int | None = None
    max_epoch_ms: int | None = None

    def add(self, epoch_ms: int) -> None:
        if self.count == 0:
            self.first_epoch_ms = epoch_ms
            self.last_epoch_ms = epoch_ms
            self.min_epoch_ms = epoch_ms
            self.max_epoch_ms = epoch_ms
        else:
            self.last_epoch_ms = epoch_ms
            self.min_epoch_ms = min(self.min_epoch_ms, epoch_ms)
            self.max_epoch_ms = max(self.max_epoch_ms, epoch_ms)
        self.count += 1

    @property
    def duration_s(self) -> float:
        if self.min_epoch_ms is None or self.max_epoch_ms is None:
            return 0.0
        return max(0.0, (self.max_epoch_ms - self.min_epoch_ms) / 1000.0)

    @property
    def actual_hz(self) -> float:
        if self.duration_s <= 0 or self.count <= 1:
            return 0.0
        return (self.count - 1) / self.duration_s


def format_bytes(n: float) -> str:
    mib = n / (1024 * 1024)
    mb = n / 1_000_000
    return f"{n:,.0f} bytes ({mb:,.2f} MB, {mib:,.2f} MiB)"


def format_duration(seconds: float) -> str:
    if math.isinf(seconds):
        return "infinite/unknown"

    hours = seconds / 3600.0
    days = hours / 24.0

    if hours < 1:
        return f"{seconds / 60.0:.2f} min"
    if hours < 48:
        return f"{hours:.2f} hr"
    return f"{hours:.2f} hr ({days:.2f} days)"


def read_csv(path: str) -> tuple[dict[str, SensorStats], int, int, int]:
    required = {"epoch_ms", "sensor", "x", "y", "z"}

    stats: dict[str, SensorStats] = defaultdict(SensorStats)
    total_rows = 0
    global_min_epoch_ms: int | None = None
    global_max_epoch_ms: int | None = None

    with open(path, "r", newline="") as f:
        reader = csv.DictReader(f)

        if reader.fieldnames is None:
            raise ValueError("CSV appears to be empty or missing a header.")

        fieldnames = {name.strip() for name in reader.fieldnames}
        missing = required - fieldnames
        if missing:
            raise ValueError(f"CSV is missing required columns: {sorted(missing)}")

        for line_num, row in enumerate(reader, start=2):
            try:
                epoch_ms = int(float(row["epoch_ms"]))
            except ValueError:
                raise ValueError(
                    f"Invalid epoch_ms on line {line_num}: {row['epoch_ms']!r}"
                )

            sensor = row["sensor"].strip()

            if sensor not in ENTRIES_PER_SAMPLE:
                known = ", ".join(sorted(ENTRIES_PER_SAMPLE))
                raise ValueError(
                    f"Unknown sensor type on line {line_num}: {sensor!r}. "
                    f"Known sensor types: {known}"
                )

            stats[sensor].add(epoch_ms)
            total_rows += 1

            if global_min_epoch_ms is None:
                global_min_epoch_ms = epoch_ms
                global_max_epoch_ms = epoch_ms
            else:
                global_min_epoch_ms = min(global_min_epoch_ms, epoch_ms)
                global_max_epoch_ms = max(global_max_epoch_ms, epoch_ms)

    if total_rows == 0:
        raise ValueError("CSV has a header but no data rows.")

    assert global_min_epoch_ms is not None
    assert global_max_epoch_ms is not None

    return stats, total_rows, global_min_epoch_ms, global_max_epoch_ms


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Estimate MetaMotion S log memory usage from a CSV with epoch_ms,sensor,x,y,z."
    )
    parser.add_argument("csv_path", help="Path to CSV file.")
    parser.add_argument(
        "--hz",
        type=float,
        required=True,
        help="Nominal sample rate per sensor, e.g. 25, 50, or 100.",
    )
    parser.add_argument(
        "--goal-hours",
        type=float,
        default=8.0,
        help="Target session length in hours. Default: 8.",
    )
    parser.add_argument(
        "--capacity-entries",
        type=int,
        default=MMS_CAPACITY_ENTRIES,
        help=f"Usable MMS logger capacity in entries. Default: {MMS_CAPACITY_ENTRIES}.",
    )

    args = parser.parse_args()

    if args.hz <= 0:
        print("ERROR: --hz must be greater than 0.", file=sys.stderr)
        return 2

    if args.goal_hours <= 0:
        print("ERROR: --goal-hours must be greater than 0.", file=sys.stderr)
        return 2

    try:
        stats, total_rows, global_min_ms, global_max_ms = read_csv(args.csv_path)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    global_duration_s = (global_max_ms - global_min_ms) / 1000.0
    goal_s = args.goal_hours * 3600.0

    used_entries = 0
    for sensor, s in stats.items():
        used_entries += s.count * ENTRIES_PER_SAMPLE[sensor]

    used_bytes = used_entries * LOG_ENTRY_BYTES

    observed_entry_rate = (
        used_entries / global_duration_s if global_duration_s > 0 else 0.0
    )
    observed_fill_time_s = (
        args.capacity_entries / observed_entry_rate
        if observed_entry_rate > 0
        else float("inf")
    )

    sensors_present = sorted(stats.keys())
    nominal_entries_per_tick = sum(ENTRIES_PER_SAMPLE[s] for s in sensors_present)
    nominal_entry_rate = args.hz * nominal_entries_per_tick
    nominal_fill_time_s = args.capacity_entries / nominal_entry_rate

    goal_entries_nominal = nominal_entry_rate * goal_s
    goal_bytes_nominal = goal_entries_nominal * LOG_ENTRY_BYTES
    goal_capacity_pct_nominal = 100.0 * goal_entries_nominal / args.capacity_entries

    goal_entries_observed = observed_entry_rate * goal_s
    goal_bytes_observed = goal_entries_observed * LOG_ENTRY_BYTES
    goal_capacity_pct_observed = 100.0 * goal_entries_observed / args.capacity_entries

    capacity_bytes = args.capacity_entries * LOG_ENTRY_BYTES

    print()
    print("MetaMotion S Memory Estimate")
    print("=" * 32)
    print(f"CSV file:              {args.csv_path}")
    print(f"CSV size:              {format_bytes(os.path.getsize(args.csv_path))}")
    print(f"Nominal sample rate:   {args.hz:g} Hz per sensor")
    print(f"Goal duration:         {args.goal_hours:g} hr")
    print()
    print("Logger capacity model")
    print("-" * 32)
    print(f"Capacity entries:      {args.capacity_entries:,}")
    print(f"Entry size:            {LOG_ENTRY_BYTES} bytes")
    print(f"Capacity bytes:        {format_bytes(capacity_bytes)}")
    print()
    print("Detected data")
    print("-" * 32)
    print(f"Total CSV rows:        {total_rows:,}")
    print(f"Sensors present:       {', '.join(sensors_present)}")
    print(f"First epoch_ms:        {global_min_ms}")
    print(f"Last epoch_ms:         {global_max_ms}")
    print(f"Logged duration:       {format_duration(global_duration_s)}")
    print(f"Used entries:          {used_entries:,}")
    print(f"Used logger bytes:     {format_bytes(used_bytes)}")
    print(f"Capacity used:         {100.0 * used_entries / args.capacity_entries:.3f}%")
    print()
    print("Per-sensor stats")
    print("-" * 32)

    for sensor in sensors_present:
        s = stats[sensor]
        entries = s.count * ENTRIES_PER_SAMPLE[sensor]
        expected_count_global = args.hz * global_duration_s
        missing_vs_nominal = expected_count_global - s.count

        print(f"{sensor}:")
        print(f"  rows/samples:        {s.count:,}")
        print(f"  entries/sample:      {ENTRIES_PER_SAMPLE[sensor]}")
        print(f"  total entries:       {entries:,}")
        print(f"  sensor duration:     {format_duration(s.duration_s)}")
        print(f"  actual sample rate:  {s.actual_hz:.3f} Hz")

        if global_duration_s > 0:
            print(f"  nominal expected:    {expected_count_global:,.1f} samples")
            print(f"  delta vs nominal:    {-missing_vs_nominal:,.1f} samples")
        print()

    print("Observed-rate estimate from CSV")
    print("-" * 32)
    print(f"Observed entry rate:   {observed_entry_rate:,.3f} entries/sec")
    print(f"Estimated fill time:   {format_duration(observed_fill_time_s)}")
    print(f"{args.goal_hours:g} hr entries:        {goal_entries_observed:,.0f}")
    print(f"{args.goal_hours:g} hr bytes:          {format_bytes(goal_bytes_observed)}")
    print(f"{args.goal_hours:g} hr capacity use:   {goal_capacity_pct_observed:.3f}%")
    print()

    print("Nominal-rate estimate from --hz")
    print("-" * 32)
    print(f"Entries per sample tick: {nominal_entries_per_tick}")
    print(f"Nominal entry rate:      {nominal_entry_rate:,.3f} entries/sec")
    print(f"Estimated fill time:     {format_duration(nominal_fill_time_s)}")
    print(f"{args.goal_hours:g} hr entries:          {goal_entries_nominal:,.0f}")
    print(
        f"{args.goal_hours:g} hr bytes:            {format_bytes(goal_bytes_nominal)}"
    )
    print(f"{args.goal_hours:g} hr capacity use:     {goal_capacity_pct_nominal:.3f}%")
    print()

    if goal_capacity_pct_nominal < 80.0:
        print("Result: PASS")
        print(
            f"At {args.hz:g} Hz with {', '.join(sensors_present)}, "
            f"{args.goal_hours:g} hours should fit comfortably."
        )
    elif goal_capacity_pct_nominal < 100.0:
        print("Result: MARGINAL")
        print(
            f"At {args.hz:g} Hz, {args.goal_hours:g} hours fits on paper, "
            "but the margin is tight."
        )
    else:
        print("Result: FAIL")
        print(
            f"At {args.hz:g} Hz, {args.goal_hours:g} hours exceeds the modeled logger capacity."
        )

    print()
    print("Note:")
    print(
        "This models MMS flash usage by logger entries, not CSV file size. "
        "Accel XYZ and gyro XYZ are treated as separate log streams sharing the same "
        "logger memory pool."
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
