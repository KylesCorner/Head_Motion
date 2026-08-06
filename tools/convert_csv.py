#!/usr/bin/env python3
"""Convert a long-format MMS+ IMU CSV to an Xsens-style wide CSV.

Input columns:
    epoch_ms,elapsed_ms,sensor,x,y,z

The ``elapsed_ms`` column is optional. Older files containing only
``epoch_ms,sensor,x,y,z`` are also supported; elapsed time is then calculated
from the first matched gyro sample.

Expected sensor names:
    accel_g
    gyro_dps

Output columns:
    PacketCounter,SampleTimeFine,Euler_X,Euler_Y,Euler_Z,
    Acc_X,Acc_Y,Acc_Z,Gyr_X,Gyr_Y,Gyr_Z,elapsed_ms

Conversion behavior:
    * SampleTimeFine is copied from the gyro row's epoch_ms value.
    * Euler columns are numeric zero placeholders.
    * Acc_X/Y/Z and Gyr_X/Y/Z contain real MMS+ samples.
    * Accel and gyro samples are paired one-to-one by nearest timestamp within
      a configurable tolerance.
    * No interpolation, resampling, filtering, or unit conversion is performed.
    * The input is streamed and never loaded entirely into memory.
    * Battery CSV files are not read or modified.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import math
import os
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence


ACCEL_SENSOR = "accel_g"
GYRO_SENSOR = "gyro_dps"
REQUIRED_COLUMNS = ("epoch_ms", "sensor", "x", "y", "z")
ELAPSED_COLUMN_ALIASES = ("elapsed_ms", "elapsed_time")
OUTPUT_HEADER = (
    "PacketCounter",
    "SampleTimeFine",
    "Euler_X",
    "Euler_Y",
    "Euler_Z",
    "Acc_X",
    "Acc_Y",
    "Acc_Z",
    "Gyr_X",
    "Gyr_Y",
    "Gyr_Z",
    "elapsed_ms",
)
DEFAULT_INFERENCE_SAMPLES = 4096
DEFAULT_FALLBACK_TOLERANCE_MS = 20


class ConversionError(RuntimeError):
    """Raised when the input data cannot be converted safely."""


@dataclass(frozen=True, slots=True)
class ImuSample:
    epoch_ms: int
    epoch_text: str
    elapsed_text: str | None
    x_text: str
    y_text: str
    z_text: str
    line_number: int


@dataclass(slots=True)
class ConversionStats:
    output_rows: int = 0
    unmatched_accel: int = 0
    unmatched_gyro: int = 0
    maximum_pair_delta_ms: int = 0
    sum_pair_delta_ms: int = 0

    @property
    def mean_pair_delta_ms(self) -> float:
        if self.output_rows == 0:
            return 0.0
        return self.sum_pair_delta_ms / self.output_rows


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert a long-format MMS+ imu.csv into an Xsens-style wide CSV "
            "without resampling or interpolation."
        )
    )
    parser.add_argument("input_csv", type=Path, help="Path to the MMS+ imu.csv")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output path (default: <input stem>_xsens.csv)",
    )
    parser.add_argument(
        "--tolerance-ms",
        type=int,
        help=(
            "Maximum accel/gyro timestamp difference. By default, the tool "
            "infers approximately one sample period from the input."
        ),
    )
    parser.add_argument(
        "--packet-start",
        type=int,
        default=0,
        help="Initial PacketCounter value (default: 0)",
    )
    parser.add_argument(
        "--progress-every",
        type=int,
        default=1_000_000,
        help=(
            "Print progress after this many output rows; use 0 to disable "
            "(default: 1000000)"
        ),
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Replace an existing output file",
    )

    args = parser.parse_args()

    if args.tolerance_ms is not None and args.tolerance_ms < 0:
        parser.error("--tolerance-ms cannot be negative")
    if args.packet_start < 0:
        parser.error("--packet-start cannot be negative")
    if args.progress_every < 0:
        parser.error("--progress-every cannot be negative")

    return args


def default_output_path(input_path: Path) -> Path:
    return input_path.with_name(f"{input_path.stem}_xsens.csv")


def parse_epoch_ms(text: str, *, line_number: int) -> int:
    stripped = text.strip()
    try:
        return int(stripped)
    except ValueError:
        try:
            value = float(stripped)
        except ValueError as exc:
            raise ConversionError(
                f"line {line_number}: invalid epoch_ms value {text!r}"
            ) from exc

        if not math.isfinite(value) or not value.is_integer():
            raise ConversionError(
                f"line {line_number}: epoch_ms must be an integer, got {text!r}"
            )
        return int(value)


def validate_numeric(text: str, *, column: str, line_number: int) -> str:
    stripped = text.strip()
    try:
        value = float(stripped)
    except ValueError as exc:
        raise ConversionError(
            f"line {line_number}: invalid {column} value {text!r}"
        ) from exc

    if not math.isfinite(value):
        raise ConversionError(
            f"line {line_number}: {column} must be finite, got {text!r}"
        )
    return stripped


def find_elapsed_column(header_map: dict[str, int]) -> str | None:
    for name in ELAPSED_COLUMN_ALIASES:
        if name in header_map:
            return name
    return None


def iter_sensor_samples(csv_path: Path, sensor_name: str) -> Iterator[ImuSample]:
    """Yield one sensor stream while validating that its timestamps are sorted."""
    try:
        handle = csv_path.open("r", encoding="utf-8-sig", newline="")
    except OSError as exc:
        raise ConversionError(f"failed to open input CSV: {exc}") from exc

    with handle:
        reader = csv.reader(handle)
        try:
            header = next(reader)
        except StopIteration as exc:
            raise ConversionError("input CSV is empty") from exc

        normalized_header = [name.strip() for name in header]
        header_map = {name: index for index, name in enumerate(normalized_header)}

        missing = [name for name in REQUIRED_COLUMNS if name not in header_map]
        if missing:
            raise ConversionError(
                "input CSV is missing required column(s): " + ", ".join(missing)
            )

        elapsed_column = find_elapsed_column(header_map)
        maximum_index = max(
            header_map[name]
            for name in REQUIRED_COLUMNS
            if name in header_map
        )
        if elapsed_column is not None:
            maximum_index = max(maximum_index, header_map[elapsed_column])

        previous_epoch: int | None = None

        for line_number, row in enumerate(reader, start=2):
            if not row or all(not value.strip() for value in row):
                continue
            if len(row) <= maximum_index:
                raise ConversionError(
                    f"line {line_number}: row has {len(row)} columns; "
                    f"expected at least {maximum_index + 1}"
                )

            sensor = row[header_map["sensor"]].strip()
            if sensor != sensor_name:
                continue

            epoch_text = row[header_map["epoch_ms"]].strip()
            epoch_ms = parse_epoch_ms(epoch_text, line_number=line_number)

            if previous_epoch is not None and epoch_ms < previous_epoch:
                raise ConversionError(
                    f"line {line_number}: {sensor_name} timestamps are not sorted "
                    f"({epoch_ms} follows {previous_epoch}); this streaming tool "
                    "requires each sensor stream to be chronological"
                )
            previous_epoch = epoch_ms

            elapsed_text = None
            if elapsed_column is not None:
                elapsed_text = validate_numeric(
                    row[header_map[elapsed_column]],
                    column=elapsed_column,
                    line_number=line_number,
                )

            yield ImuSample(
                epoch_ms=epoch_ms,
                epoch_text=epoch_text,
                elapsed_text=elapsed_text,
                x_text=validate_numeric(
                    row[header_map["x"]], column="x", line_number=line_number
                ),
                y_text=validate_numeric(
                    row[header_map["y"]], column="y", line_number=line_number
                ),
                z_text=validate_numeric(
                    row[header_map["z"]], column="z", line_number=line_number
                ),
                line_number=line_number,
            )


def take_prefix(
    iterator: Iterator[ImuSample], count: int
) -> tuple[list[ImuSample], Iterator[ImuSample]]:
    prefix = list(itertools.islice(iterator, count))
    return prefix, itertools.chain(prefix, iterator)


def median_positive_period_ms(samples: Sequence[ImuSample]) -> float | None:
    deltas = [
        current.epoch_ms - previous.epoch_ms
        for previous, current in zip(samples, samples[1:])
        if current.epoch_ms > previous.epoch_ms
    ]
    if not deltas:
        return None
    return float(statistics.median(deltas))


def infer_tolerance_ms(
    accel_samples: Sequence[ImuSample], gyro_samples: Sequence[ImuSample]
) -> tuple[int, str]:
    periods = [
        period
        for period in (
            median_positive_period_ms(accel_samples),
            median_positive_period_ms(gyro_samples),
        )
        if period is not None
    ]

    if not periods:
        return DEFAULT_FALLBACK_TOLERANCE_MS, "fallback"

    # Match the C++ conversion behavior: allow approximately one sample period.
    return max(1, math.ceil(max(periods))), "inferred"


def remove_window_item(window: list[ImuSample], index: int) -> ImuSample:
    sample = window[index]
    del window[index]
    return sample


def pair_samples(
    accel_samples: Iterable[ImuSample],
    gyro_samples: Iterable[ImuSample],
    tolerance_ms: int,
    stats: ConversionStats,
) -> Iterator[tuple[ImuSample, ImuSample, int]]:
    """Pair each gyro with the nearest unused accel inside the time tolerance."""
    accel_iterator = iter(accel_samples)
    next_accel = next(accel_iterator, None)
    accel_window: list[ImuSample] = []

    for gyro in gyro_samples:
        upper_bound = gyro.epoch_ms + tolerance_ms
        lower_bound = gyro.epoch_ms - tolerance_ms

        while next_accel is not None and next_accel.epoch_ms <= upper_bound:
            accel_window.append(next_accel)
            next_accel = next(accel_iterator, None)

        first_valid = 0
        while (
            first_valid < len(accel_window)
            and accel_window[first_valid].epoch_ms < lower_bound
        ):
            first_valid += 1

        if first_valid:
            stats.unmatched_accel += first_valid
            del accel_window[:first_valid]

        if not accel_window:
            stats.unmatched_gyro += 1
            continue

        candidate_index = min(
            range(len(accel_window)),
            key=lambda index: (
                abs(accel_window[index].epoch_ms - gyro.epoch_ms),
                accel_window[index].epoch_ms,
            ),
        )
        accel = accel_window[candidate_index]
        delta_ms = abs(accel.epoch_ms - gyro.epoch_ms)

        if delta_ms > tolerance_ms:
            stats.unmatched_gyro += 1
            continue

        remove_window_item(accel_window, candidate_index)
        yield accel, gyro, delta_ms

    stats.unmatched_accel += len(accel_window)
    if next_accel is not None:
        stats.unmatched_accel += 1
    stats.unmatched_accel += sum(1 for _ in accel_iterator)


def write_output(
    output_path: Path,
    accel_samples: Iterable[ImuSample],
    gyro_samples: Iterable[ImuSample],
    tolerance_ms: int,
    packet_start: int,
    progress_every: int,
) -> ConversionStats:
    stats = ConversionStats()
    temporary_path = output_path.with_name(output_path.name + ".tmp")

    if temporary_path.exists():
        temporary_path.unlink()

    elapsed_origin_ms: int | None = None

    try:
        with temporary_path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.writer(handle, lineterminator="\n")
            writer.writerow(OUTPUT_HEADER)

            for accel, gyro, delta_ms in pair_samples(
                accel_samples, gyro_samples, tolerance_ms, stats
            ):
                if elapsed_origin_ms is None:
                    elapsed_origin_ms = gyro.epoch_ms

                elapsed_text = gyro.elapsed_text
                if elapsed_text is None:
                    elapsed_text = str(gyro.epoch_ms - elapsed_origin_ms)

                writer.writerow(
                    (
                        packet_start + stats.output_rows,
                        gyro.epoch_text,
                        0,
                        0,
                        0,
                        accel.x_text,
                        accel.y_text,
                        accel.z_text,
                        gyro.x_text,
                        gyro.y_text,
                        gyro.z_text,
                        elapsed_text,
                    )
                )

                stats.output_rows += 1
                stats.sum_pair_delta_ms += delta_ms
                stats.maximum_pair_delta_ms = max(
                    stats.maximum_pair_delta_ms, delta_ms
                )

                if progress_every and stats.output_rows % progress_every == 0:
                    print(
                        f"Wrote {stats.output_rows:,} paired rows "
                        f"(unmatched accel={stats.unmatched_accel:,}, "
                        f"gyro={stats.unmatched_gyro:,})",
                        flush=True,
                    )

        if stats.output_rows == 0:
            raise ConversionError(
                "no accel/gyro pairs were found within the selected tolerance"
            )

        os.replace(temporary_path, output_path)
        return stats
    except BaseException:
        try:
            temporary_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def main() -> int:
    args = parse_args()
    input_path = args.input_csv.expanduser().resolve()
    output_path = (
        args.output.expanduser().resolve()
        if args.output is not None
        else default_output_path(input_path)
    )

    if not input_path.is_file():
        print(f"error: input file not found: {input_path}", file=sys.stderr)
        return 2
    if input_path == output_path:
        print("error: input and output paths must be different", file=sys.stderr)
        return 2
    if output_path.exists() and not args.force:
        print(
            f"error: output already exists: {output_path}\n"
            "Use --force to replace it.",
            file=sys.stderr,
        )
        return 2

    output_path.parent.mkdir(parents=True, exist_ok=True)

    try:
        accel_iterator = iter_sensor_samples(input_path, ACCEL_SENSOR)
        gyro_iterator = iter_sensor_samples(input_path, GYRO_SENSOR)

        accel_prefix, accel_samples = take_prefix(
            accel_iterator, DEFAULT_INFERENCE_SAMPLES
        )
        gyro_prefix, gyro_samples = take_prefix(
            gyro_iterator, DEFAULT_INFERENCE_SAMPLES
        )

        if not accel_prefix:
            raise ConversionError(f"no {ACCEL_SENSOR} rows found")
        if not gyro_prefix:
            raise ConversionError(f"no {GYRO_SENSOR} rows found")

        if args.tolerance_ms is None:
            tolerance_ms, tolerance_source = infer_tolerance_ms(
                accel_prefix, gyro_prefix
            )
        else:
            tolerance_ms = args.tolerance_ms
            tolerance_source = "command line"

        has_elapsed = any(sample.elapsed_text is not None for sample in gyro_prefix)

        print(f"Input: {input_path}")
        print(f"Output: {output_path}")
        print(
            f"Pairing tolerance: {tolerance_ms} ms ({tolerance_source}); "
            "no resampling or interpolation"
        )
        if not has_elapsed:
            print(
                "Input has no elapsed_ms column; output elapsed_ms will begin "
                "at zero on the first paired gyro sample."
            )

        stats = write_output(
            output_path=output_path,
            accel_samples=accel_samples,
            gyro_samples=gyro_samples,
            tolerance_ms=tolerance_ms,
            packet_start=args.packet_start,
            progress_every=args.progress_every,
        )

    except (ConversionError, OSError, csv.Error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print("Conversion complete.")
    print(f"Paired rows written: {stats.output_rows:,}")
    print(f"Unmatched accel samples: {stats.unmatched_accel:,}")
    print(f"Unmatched gyro samples: {stats.unmatched_gyro:,}")
    print(f"Maximum pair delta: {stats.maximum_pair_delta_ms} ms")
    print(f"Mean pair delta: {stats.mean_pair_delta_ms:.3f} ms")
    print(f"Xsens-compatible CSV: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())