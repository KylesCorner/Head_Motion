from pathlib import Path

script = r'''#!/usr/bin/env python3
"""
Analyze MMS+ data recorded on a Moog Gen 3 platform.

The script:
  1. Reads an MMS+ CSV supplied as a positional command-line argument.
  2. Detects the beginning of platform movement from the first large,
     sustained gyroscope event caused by the platform moving to neutral.
  3. Uses the pre-movement interval as the stationary noise reference.
  4. Separates low-frequency platform motion from higher-frequency vibration.
  5. Produces interactive hvPlot plots and a small metrics table.

Expected CSV columns:
    epoch_ms,sensor,x,y,z

Expected sensor labels:
    accel_g
    gyro_dps
"""

from __future__ import annotations

import argparse
import sys
import webbrowser
from dataclasses import dataclass
from pathlib import Path

import hvplot.pandas  # noqa: F401 - registers the .hvplot accessor
import numpy as np
import pandas as pd
import panel as pn
from scipy.integrate import cumulative_trapezoid
from scipy.signal import butter, find_peaks, sosfiltfilt, welch


REQUIRED_COLUMNS = {"epoch_ms", "sensor", "x", "y", "z"}
AXES = ("x", "y", "z")


@dataclass(frozen=True)
class StartDetection:
    start_time_s: float
    peak_time_s: float
    peak_activity_dps: float
    high_threshold_dps: float
    low_threshold_dps: float
    baseline_median_dps: float
    baseline_sigma_dps: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Detect the Moog platform movement start from MMS+ gyroscope data "
            "and create an interactive motion/vibration report."
        )
    )

    parser.add_argument(
        "csv_file",
        type=Path,
        help="Input MMS+ CSV file.",
    )

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output HTML report. Default: <input_stem>_moog_report.html",
    )

    parser.add_argument(
        "--metrics-output",
        type=Path,
        help="Optional CSV output for stationary/moving vibration metrics.",
    )

    parser.add_argument(
        "--sample-rate",
        type=float,
        default=200.0,
        help="Expected MMS+ sample rate in Hz. Default: 200.",
    )

    parser.add_argument(
        "--auto-sample-rate",
        action="store_true",
        help="Estimate each sensor's sample rate from epoch_ms timestamps.",
    )

    parser.add_argument(
        "--start-time",
        type=float,
        default=None,
        help=(
            "Manual movement-start time in elapsed seconds. "
            "When omitted, the start is detected from gyro activity."
        ),
    )

    parser.add_argument(
        "--search-window",
        type=float,
        default=15.0,
        help=(
            "Search for the neutral-position gyro event within this many "
            "seconds from the start of the recording. Default: 15."
        ),
    )

    parser.add_argument(
        "--baseline-seconds",
        type=float,
        default=2.0,
        help=(
            "Initial interval used to estimate stationary gyro noise. "
            "Default: 2 seconds."
        ),
    )

    parser.add_argument(
        "--gyro-threshold-sigma",
        type=float,
        default=8.0,
        help=(
            "High detection threshold in robust standard deviations above "
            "the gyro noise floor. Default: 8."
        ),
    )

    parser.add_argument(
        "--gyro-min-threshold",
        type=float,
        default=3.0,
        help=(
            "Minimum gyro activity threshold in deg/s. This prevents an "
            "extremely quiet baseline from producing a tiny threshold. "
            "Default: 3 deg/s."
        ),
    )

    parser.add_argument(
        "--start-sustain-ms",
        type=float,
        default=100.0,
        help=(
            "Minimum duration of gyro activity required for start detection. "
            "Default: 100 ms."
        ),
    )

    parser.add_argument(
        "--stationary-guard",
        type=float,
        default=0.25,
        help=(
            "Exclude this many seconds immediately before detected movement "
            "from the stationary reference. Default: 0.25."
        ),
    )

    parser.add_argument(
        "--movement-end",
        type=float,
        default=None,
        help=(
            "Optional end of the moving interval in elapsed seconds. "
            "Default: end of recording."
        ),
    )

    parser.add_argument(
        "--motion-cutoff",
        type=float,
        default=10.0,
        help=(
            "Low-pass cutoff used for movement verification. Default: 10 Hz."
        ),
    )

    parser.add_argument(
        "--vibration-low",
        type=float,
        default=1.0,
        help="Lower vibration-band cutoff. Default: 1 Hz.",
    )

    parser.add_argument(
        "--vibration-high",
        type=float,
        default=80.0,
        help=(
            "Upper vibration-band cutoff. Default: 80 Hz. "
            "For 200 Hz sampling, Nyquist is 100 Hz."
        ),
    )

    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="Do not open the generated HTML report automatically.",
    )

    return parser.parse_args()


def load_csv(csv_file: Path) -> pd.DataFrame:
    if not csv_file.is_file():
        raise FileNotFoundError(f"CSV file not found: {csv_file}")

    data = pd.read_csv(csv_file)

    missing = REQUIRED_COLUMNS - set(data.columns)
    if missing:
        raise ValueError(
            "CSV is missing required columns: "
            + ", ".join(sorted(missing))
        )

    data["sensor"] = data["sensor"].astype(str).str.strip()

    for column in ("epoch_ms", "x", "y", "z"):
        data[column] = pd.to_numeric(data[column], errors="coerce")

    invalid_rows = data[["epoch_ms", "x", "y", "z"]].isna().any(axis=1)
    if invalid_rows.any():
        print(
            f"Warning: dropping {int(invalid_rows.sum())} invalid rows.",
            file=sys.stderr,
        )
        data = data.loc[~invalid_rows].copy()

    if data.empty:
        raise ValueError("No valid sensor rows remain after loading the CSV.")

    data = data.sort_values("epoch_ms").reset_index(drop=True)

    first_epoch_ms = float(data["epoch_ms"].min())
    data["time_s"] = (data["epoch_ms"] - first_epoch_ms) / 1000.0
    data["timestamp"] = pd.to_datetime(data["epoch_ms"], unit="ms")

    return data


def select_sensor(data: pd.DataFrame, sensor_name: str) -> pd.DataFrame:
    sensor_data = (
        data.loc[data["sensor"] == sensor_name]
        .sort_values("time_s")
        .drop_duplicates(subset="epoch_ms", keep="first")
        .reset_index(drop=True)
    )

    if sensor_data.empty:
        raise ValueError(f"No rows labeled '{sensor_name}' were found.")

    return sensor_data


def estimate_sample_rate(data: pd.DataFrame) -> float:
    dt = np.diff(data["time_s"].to_numpy(dtype=float))
    dt = dt[np.isfinite(dt) & (dt > 0)]

    if len(dt) == 0:
        raise ValueError("Cannot estimate sample rate from the timestamps.")

    return float(1.0 / np.median(dt))


def timestamp_summary(
    data: pd.DataFrame,
    expected_sample_rate: float,
    sensor_name: str,
) -> str:
    measured_rate = estimate_sample_rate(data)
    dt = np.diff(data["time_s"].to_numpy(dtype=float))
    expected_dt = 1.0 / expected_sample_rate
    gaps = int(np.count_nonzero(dt > 1.5 * expected_dt))

    return (
        f"{sensor_name}: measured median rate {measured_rate:.3f} Hz; "
        f"{gaps} gaps larger than {1.5 * expected_dt * 1000.0:.2f} ms"
    )


def resample_uniform(
    data: pd.DataFrame,
    sample_rate: float,
) -> pd.DataFrame:
    """
    Interpolate onto a uniform grid.

    This handles ordinary timestamp jitter. Large acquisition gaps should be
    investigated separately because interpolation cannot recreate missing data.
    """
    original_time = data["time_s"].to_numpy(dtype=float)
    period = 1.0 / sample_rate

    uniform_time = np.arange(
        original_time[0],
        original_time[-1] + period / 2.0,
        period,
    )

    result = pd.DataFrame({"time_s": uniform_time})

    for axis in AXES:
        result[axis] = np.interp(
            uniform_time,
            original_time,
            data[axis].to_numpy(dtype=float),
        )

    result["magnitude"] = np.sqrt(
        result["x"] ** 2 + result["y"] ** 2 + result["z"] ** 2
    )

    return result


def robust_sigma(values: np.ndarray) -> float:
    """
    Convert median absolute deviation to a Gaussian-equivalent sigma.
    """
    values = np.asarray(values, dtype=float)
    median = np.median(values)
    mad = np.median(np.abs(values - median))
    return float(1.4826 * mad)


def make_filter(
    sample_rate: float,
    filter_type: str,
    cutoff: float | tuple[float, float],
    order: int = 4,
) -> np.ndarray:
    nyquist = sample_rate / 2.0

    if filter_type == "bandpass":
        low, high = cutoff  # type: ignore[misc]
        if not 0.0 < low < high < nyquist:
            raise ValueError(
                f"Band-pass cutoffs must satisfy 0 < low < high < "
                f"{nyquist:.3f} Hz."
            )
        normalized = (low / nyquist, high / nyquist)
    else:
        cutoff_value = float(cutoff)  # type: ignore[arg-type]
        if not 0.0 < cutoff_value < nyquist:
            raise ValueError(
                f"Filter cutoff must be below Nyquist ({nyquist:.3f} Hz)."
            )
        normalized = cutoff_value / nyquist

    return butter(order, normalized, btype=filter_type, output="sos")


def filter_axes(data: pd.DataFrame, sos: np.ndarray) -> pd.DataFrame:
    result = data.copy()

    for axis in AXES:
        result[axis] = sosfiltfilt(
            sos,
            data[axis].to_numpy(dtype=float),
        )

    result["magnitude"] = np.sqrt(
        result["x"] ** 2 + result["y"] ** 2 + result["z"] ** 2
    )

    return result


def centered_rolling_rms(
    values: np.ndarray,
    sample_rate: float,
    window_seconds: float,
) -> np.ndarray:
    window_samples = max(3, int(round(window_seconds * sample_rate)))
    if window_samples % 2 == 0:
        window_samples += 1

    squared = pd.Series(np.square(values))
    mean_square = squared.rolling(
        window=window_samples,
        center=True,
        min_periods=1,
    ).mean()

    return np.sqrt(mean_square.to_numpy(dtype=float))


def detect_movement_start(
    gyro: pd.DataFrame,
    sample_rate: float,
    search_window_s: float,
    baseline_seconds: float,
    threshold_sigma: float,
    minimum_threshold_dps: float,
    sustain_ms: float,
) -> tuple[StartDetection, pd.DataFrame]:
    """
    Detect the large gyro event generated while the Moog moves to neutral.

    Detection strategy:
      1. Estimate per-axis bias from the initial baseline.
      2. Calculate bias-corrected gyro vector magnitude.
      3. Low-pass it at 20 Hz to suppress sample-level spikes.
      4. Calculate a 50 ms rolling RMS activity envelope.
      5. Find the strongest sustained peak in the initial search window.
      6. Walk backward from the peak to the lower hysteresis threshold to
         estimate the event onset.
    """
    if len(gyro) < 10:
        raise ValueError("Not enough gyroscope samples for start detection.")

    max_time = float(gyro["time_s"].iloc[-1])
    search_stop = min(search_window_s, max_time)

    baseline_stop = min(
        baseline_seconds,
        search_stop * 0.5,
    )
    baseline = gyro.loc[gyro["time_s"] <= baseline_stop]

    if len(baseline) < max(10, int(0.1 * sample_rate)):
        baseline = gyro.iloc[: max(10, int(0.5 * sample_rate))]

    bias = baseline[list(AXES)].median()

    corrected = gyro.copy()
    for axis in AXES:
        corrected[f"{axis}_corrected"] = corrected[axis] - float(bias[axis])

    corrected["gyro_vector_dps"] = np.sqrt(
        corrected["x_corrected"] ** 2
        + corrected["y_corrected"] ** 2
        + corrected["z_corrected"] ** 2
    )

    detection_cutoff = min(20.0, 0.4 * sample_rate)
    sos = make_filter(sample_rate, "lowpass", detection_cutoff, order=4)

    corrected["gyro_vector_filtered_dps"] = sosfiltfilt(
        sos,
        corrected["gyro_vector_dps"].to_numpy(dtype=float),
    )

    corrected["gyro_activity_dps"] = centered_rolling_rms(
        corrected["gyro_vector_filtered_dps"].to_numpy(dtype=float),
        sample_rate,
        window_seconds=0.050,
    )

    baseline_activity = corrected.loc[
        corrected["time_s"] <= baseline_stop,
        "gyro_activity_dps",
    ].to_numpy(dtype=float)

    baseline_median = float(np.median(baseline_activity))
    baseline_sigma = robust_sigma(baseline_activity)

    # Give the threshold a small non-zero robust scale in very quiet data.
    effective_sigma = max(baseline_sigma, 0.05)

    high_threshold = max(
        minimum_threshold_dps,
        baseline_median + threshold_sigma * effective_sigma,
    )

    # Hysteresis threshold used to walk back from the peak to the onset.
    low_threshold = max(
        baseline_median + 3.0 * effective_sigma,
        0.35 * high_threshold,
    )

    search = corrected.loc[
        (corrected["time_s"] >= baseline_stop)
        & (corrected["time_s"] <= search_stop)
    ].copy()

    if search.empty:
        raise ValueError("The gyro start-detection search window is empty.")

    sustain_samples = max(
        1,
        int(round((sustain_ms / 1000.0) * sample_rate)),
    )

    above = search["gyro_activity_dps"].to_numpy(dtype=float) >= high_threshold

    # A moving-sum mask requires a sustained threshold crossing.
    sustained = (
        pd.Series(above.astype(int))
        .rolling(window=sustain_samples, min_periods=sustain_samples)
        .sum()
        .to_numpy()
        >= sustain_samples
    )

    if not np.any(sustained):
        peak_value = float(search["gyro_activity_dps"].max())
        peak_time = float(
            search.loc[search["gyro_activity_dps"].idxmax(), "time_s"]
        )
        raise ValueError(
            "Automatic start detection did not find a sustained gyro event. "
            f"Strongest activity was {peak_value:.3f} deg/s at "
            f"{peak_time:.3f} s; threshold was {high_threshold:.3f} deg/s. "
            "Use --start-time to override detection or lower "
            "--gyro-threshold-sigma/--gyro-min-threshold."
        )

    candidate_indices = np.flatnonzero(sustained)

    # Find peaks in the search envelope and choose the first strong peak
    # associated with a sustained threshold crossing. The neutral movement is
    # expected to be the first prominent event.
    activity = search["gyro_activity_dps"].to_numpy(dtype=float)
    peaks, properties = find_peaks(
        activity,
        height=high_threshold,
        distance=max(1, int(round(0.1 * sample_rate))),
    )

    sustained_start_index = int(candidate_indices[0])

    valid_peaks = peaks[peaks >= max(0, sustained_start_index - sustain_samples)]
    if len(valid_peaks) > 0:
        peak_local_index = int(valid_peaks[0])
    else:
        # Fall back to the strongest point shortly after the sustained onset.
        search_end = min(
            len(search),
            sustained_start_index + int(round(2.0 * sample_rate)),
        )
        peak_local_index = sustained_start_index + int(
            np.argmax(activity[sustained_start_index:search_end])
        )

    # Walk backward from the detected peak until activity falls below the
    # lower hysteresis threshold. This marks event onset, not maximum motion.
    onset_local_index = peak_local_index
    while (
        onset_local_index > 0
        and activity[onset_local_index - 1] > low_threshold
    ):
        onset_local_index -= 1

    # Require at least a few consecutive samples below threshold before onset
    # to reduce sensitivity to isolated baseline fluctuations.
    quiet_samples = max(1, int(round(0.025 * sample_rate)))
    while onset_local_index > quiet_samples:
        previous = activity[
            onset_local_index - quiet_samples : onset_local_index
        ]
        if np.all(previous <= low_threshold):
            break
        onset_local_index -= 1

    onset_row = search.iloc[onset_local_index]
    peak_row = search.iloc[peak_local_index]

    detection = StartDetection(
        start_time_s=float(onset_row["time_s"]),
        peak_time_s=float(peak_row["time_s"]),
        peak_activity_dps=float(peak_row["gyro_activity_dps"]),
        high_threshold_dps=float(high_threshold),
        low_threshold_dps=float(low_threshold),
        baseline_median_dps=float(baseline_median),
        baseline_sigma_dps=float(baseline_sigma),
    )

    return detection, corrected


def select_interval(
    data: pd.DataFrame,
    start_s: float,
    stop_s: float,
    label: str,
) -> pd.DataFrame:
    selected = data.loc[
        (data["time_s"] >= start_s)
        & (data["time_s"] <= stop_s)
    ].copy()

    if selected.empty:
        raise ValueError(
            f"No {label} samples were found between "
            f"{start_s:.3f} and {stop_s:.3f} seconds."
        )

    return selected


def calculate_psd(
    data: pd.DataFrame,
    sample_rate: float,
    condition: str,
) -> pd.DataFrame:
    frames: list[pd.DataFrame] = []

    nperseg = min(4096, len(data))
    if nperseg < 64:
        raise ValueError(
            f"Not enough {condition.lower()} samples for Welch PSD."
        )

    for axis in AXES:
        frequency, psd = welch(
            data[axis].to_numpy(dtype=float),
            fs=sample_rate,
            window="hann",
            nperseg=nperseg,
            noverlap=nperseg // 2,
            detrend="constant",
            scaling="density",
        )

        frames.append(
            pd.DataFrame(
                {
                    "frequency_hz": frequency,
                    "psd": psd,
                    "axis": axis.upper(),
                    "condition": condition,
                    "series": f"{condition} {axis.upper()}",
                }
            )
        )

    return pd.concat(frames, ignore_index=True)


def dominant_frequency(
    values: np.ndarray,
    sample_rate: float,
    low_hz: float,
    high_hz: float,
) -> float:
    nperseg = min(4096, len(values))
    if nperseg < 64:
        return float("nan")

    frequency, psd = welch(
        values,
        fs=sample_rate,
        window="hann",
        nperseg=nperseg,
        noverlap=nperseg // 2,
        detrend="constant",
        scaling="density",
    )

    band = (frequency >= low_hz) & (frequency <= high_hz)
    if not np.any(band):
        return float("nan")

    band_f = frequency[band]
    band_psd = psd[band]
    return float(band_f[int(np.argmax(band_psd))])


def calculate_vibration_metrics(
    stationary: pd.DataFrame,
    moving: pd.DataFrame,
    sample_rate: float,
    sensor_name: str,
    low_hz: float,
    high_hz: float,
) -> pd.DataFrame:
    rows: list[dict[str, float | str]] = []

    for axis in AXES:
        stationary_values = stationary[axis].to_numpy(dtype=float)
        moving_values = moving[axis].to_numpy(dtype=float)

        stationary_rms = float(np.sqrt(np.mean(stationary_values**2)))
        moving_rms = float(np.sqrt(np.mean(moving_values**2)))

        noise_corrected_rms = float(
            np.sqrt(max(moving_rms**2 - stationary_rms**2, 0.0))
        )

        ratio = (
            moving_rms / stationary_rms
            if stationary_rms > 0.0
            else float("inf")
        )

        rows.append(
            {
                "sensor": sensor_name,
                "axis": axis.upper(),
                "stationary_rms": stationary_rms,
                "moving_rms": moving_rms,
                "moving_to_stationary_ratio": ratio,
                "noise_corrected_rms": noise_corrected_rms,
                "moving_dominant_frequency_hz": dominant_frequency(
                    moving_values,
                    sample_rate,
                    low_hz,
                    high_hz,
                ),
            }
        )

    return pd.DataFrame(rows)


def integrate_gyro(
    gyro_motion: pd.DataFrame,
    stationary_reference: pd.DataFrame,
    movement_start_s: float,
) -> pd.DataFrame:
    result = gyro_motion.copy()
    bias = stationary_reference[list(AXES)].median()

    for axis in AXES:
        result[f"{axis}_corrected"] = result[axis] - float(bias[axis])

    # Integrate relative to the detected movement start. Pre-start angle is
    # retained as zero so the plot is easy to interpret.
    post_start = result["time_s"] >= movement_start_s
    post = result.loc[post_start].copy()

    if len(post) < 2:
        raise ValueError("Not enough post-start gyro data to integrate.")

    time_values = post["time_s"].to_numpy(dtype=float)

    for axis in AXES:
        angle = cumulative_trapezoid(
            post[f"{axis}_corrected"].to_numpy(dtype=float),
            time_values,
            initial=0.0,
        )
        result[f"{axis}_angle_deg"] = 0.0
        result.loc[post_start, f"{axis}_angle_deg"] = angle

    return result


def vertical_marker(start_time_s: float):
    import holoviews as hv

    return hv.VLine(start_time_s).opts(
        line_dash="dashed",
        line_width=2,
    )


def time_plot(
    data: pd.DataFrame,
    title: str,
    ylabel: str,
    movement_start_s: float,
    columns: list[str] | tuple[str, ...] = AXES,
):
    plot = data.hvplot.line(
        x="time_s",
        y=list(columns),
        title=title,
        xlabel="Elapsed time (s)",
        ylabel=ylabel,
        responsive=True,
        height=360,
        line_width=1.2,
        legend="top_right",
        grid=True,
    )

    return plot * vertical_marker(movement_start_s)


def detection_plot(
    detection_data: pd.DataFrame,
    detection: StartDetection,
):
    import holoviews as hv

    plot = detection_data.hvplot.line(
        x="time_s",
        y="gyro_activity_dps",
        title="Automatic movement-start detection",
        xlabel="Elapsed time (s)",
        ylabel="Gyro activity envelope (deg/s)",
        responsive=True,
        height=360,
        line_width=1.4,
        grid=True,
    )

    high_line = hv.HLine(detection.high_threshold_dps).opts(
        line_dash="dotted",
        line_width=1.5,
    )
    low_line = hv.HLine(detection.low_threshold_dps).opts(
        line_dash="dotdash",
        line_width=1.2,
    )
    start_line = vertical_marker(detection.start_time_s)
    peak_line = hv.VLine(detection.peak_time_s).opts(
        line_dash="dotted",
        line_width=1.5,
    )

    return plot * high_line * low_line * start_line * peak_line


def psd_plot(
    psd: pd.DataFrame,
    title: str,
    ylabel: str,
    high_hz: float,
):
    selected = psd.loc[
        (psd["frequency_hz"] > 0.0)
        & (psd["frequency_hz"] <= high_hz)
    ]

    return selected.hvplot.line(
        x="frequency_hz",
        y="psd",
        by="series",
        title=title,
        xlabel="Frequency (Hz)",
        ylabel=ylabel,
        logy=True,
        responsive=True,
        height=420,
        line_width=1.2,
        legend="right",
        grid=True,
    )


def main() -> int:
    args = parse_args()

    try:
        raw = load_csv(args.csv_file)

        accel_raw = select_sensor(raw, "accel_g")
        gyro_raw = select_sensor(raw, "gyro_dps")

        if args.auto_sample_rate:
            accel_rate = estimate_sample_rate(accel_raw)
            gyro_rate = estimate_sample_rate(gyro_raw)
        else:
            accel_rate = args.sample_rate
            gyro_rate = args.sample_rate

        if accel_rate <= 0.0 or gyro_rate <= 0.0:
            raise ValueError("Sample rates must be positive.")

        minimum_nyquist = min(accel_rate, gyro_rate) / 2.0

        if args.vibration_high >= minimum_nyquist:
            raise ValueError(
                f"--vibration-high must be below the lowest Nyquist "
                f"frequency ({minimum_nyquist:.3f} Hz)."
            )

        accel = resample_uniform(accel_raw, accel_rate)
        gyro = resample_uniform(gyro_raw, gyro_rate)

        if args.start_time is None:
            detection, detection_data = detect_movement_start(
                gyro=gyro,
                sample_rate=gyro_rate,
                search_window_s=args.search_window,
                baseline_seconds=args.baseline_seconds,
                threshold_sigma=args.gyro_threshold_sigma,
                minimum_threshold_dps=args.gyro_min_threshold,
                sustain_ms=args.start_sustain_ms,
            )
            movement_start_s = detection.start_time_s
            start_source = "Automatically detected from neutral-position gyro event"
        else:
            movement_start_s = args.start_time

            # Still calculate an envelope for display, but do not use its
            # detected result.
            try:
                auto_detection, detection_data = detect_movement_start(
                    gyro=gyro,
                    sample_rate=gyro_rate,
                    search_window_s=args.search_window,
                    baseline_seconds=args.baseline_seconds,
                    threshold_sigma=args.gyro_threshold_sigma,
                    minimum_threshold_dps=args.gyro_min_threshold,
                    sustain_ms=args.start_sustain_ms,
                )
                detection = StartDetection(
                    start_time_s=movement_start_s,
                    peak_time_s=auto_detection.peak_time_s,
                    peak_activity_dps=auto_detection.peak_activity_dps,
                    high_threshold_dps=auto_detection.high_threshold_dps,
                    low_threshold_dps=auto_detection.low_threshold_dps,
                    baseline_median_dps=auto_detection.baseline_median_dps,
                    baseline_sigma_dps=auto_detection.baseline_sigma_dps,
                )
            except ValueError:
                detection_data = gyro.copy()
                detection_data["gyro_activity_dps"] = gyro["magnitude"]
                detection = StartDetection(
                    start_time_s=movement_start_s,
                    peak_time_s=movement_start_s,
                    peak_activity_dps=float("nan"),
                    high_threshold_dps=float("nan"),
                    low_threshold_dps=float("nan"),
                    baseline_median_dps=float("nan"),
                    baseline_sigma_dps=float("nan"),
                )

            start_source = "Manual --start-time override"

        recording_stop_s = min(
            float(accel["time_s"].iloc[-1]),
            float(gyro["time_s"].iloc[-1]),
        )

        movement_end_s = (
            recording_stop_s
            if args.movement_end is None
            else min(args.movement_end, recording_stop_s)
        )

        if movement_end_s <= movement_start_s:
            raise ValueError(
                "Movement end must occur after the detected movement start."
            )

        stationary_stop_s = movement_start_s - args.stationary_guard
        stationary_start_s = max(
            float(accel["time_s"].iloc[0]),
            float(gyro["time_s"].iloc[0]),
        )

        if stationary_stop_s <= stationary_start_s:
            raise ValueError(
                "There is not enough pre-movement data for a stationary "
                "reference. Reduce --stationary-guard or use --start-time."
            )

        accel_vibration_sos = make_filter(
            accel_rate,
            "bandpass",
            (args.vibration_low, args.vibration_high),
        )
        gyro_vibration_sos = make_filter(
            gyro_rate,
            "bandpass",
            (args.vibration_low, args.vibration_high),
        )
        accel_motion_sos = make_filter(
            accel_rate,
            "lowpass",
            args.motion_cutoff,
        )
        gyro_motion_sos = make_filter(
            gyro_rate,
            "lowpass",
            args.motion_cutoff,
        )

        accel_vibration = filter_axes(accel, accel_vibration_sos)
        gyro_vibration = filter_axes(gyro, gyro_vibration_sos)
        accel_motion = filter_axes(accel, accel_motion_sos)
        gyro_motion = filter_axes(gyro, gyro_motion_sos)

        stationary_accel_vibration = select_interval(
            accel_vibration,
            stationary_start_s,
            stationary_stop_s,
            "stationary accelerometer",
        )
        moving_accel_vibration = select_interval(
            accel_vibration,
            movement_start_s,
            movement_end_s,
            "moving accelerometer",
        )

        stationary_gyro_vibration = select_interval(
            gyro_vibration,
            stationary_start_s,
            stationary_stop_s,
            "stationary gyroscope",
        )
        moving_gyro_vibration = select_interval(
            gyro_vibration,
            movement_start_s,
            movement_end_s,
            "moving gyroscope",
        )

        stationary_gyro_motion = select_interval(
            gyro_motion,
            stationary_start_s,
            stationary_stop_s,
            "stationary gyro reference",
        )

        integrated_gyro = integrate_gyro(
            gyro_motion,
            stationary_gyro_motion,
            movement_start_s,
        )

        accel_psd = pd.concat(
            (
                calculate_psd(
                    stationary_accel_vibration,
                    accel_rate,
                    "Stationary",
                ),
                calculate_psd(
                    moving_accel_vibration,
                    accel_rate,
                    "Moving",
                ),
            ),
            ignore_index=True,
        )

        gyro_psd = pd.concat(
            (
                calculate_psd(
                    stationary_gyro_vibration,
                    gyro_rate,
                    "Stationary",
                ),
                calculate_psd(
                    moving_gyro_vibration,
                    gyro_rate,
                    "Moving",
                ),
            ),
            ignore_index=True,
        )

        metrics = pd.concat(
            (
                calculate_vibration_metrics(
                    stationary_accel_vibration,
                    moving_accel_vibration,
                    accel_rate,
                    "accel_g",
                    args.vibration_low,
                    args.vibration_high,
                ),
                calculate_vibration_metrics(
                    stationary_gyro_vibration,
                    moving_gyro_vibration,
                    gyro_rate,
                    "gyro_dps",
                    args.vibration_low,
                    args.vibration_high,
                ),
            ),
            ignore_index=True,
        )

    except (
        FileNotFoundError,
        ValueError,
        pd.errors.ParserError,
    ) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    output_file = (
        args.output
        if args.output is not None
        else args.csv_file.with_name(
            f"{args.csv_file.stem}_moog_report.html"
        )
    )
    output_file.parent.mkdir(parents=True, exist_ok=True)

    if args.metrics_output is not None:
        args.metrics_output.parent.mkdir(parents=True, exist_ok=True)
        metrics.to_csv(args.metrics_output, index=False)

    timestamp_checks = "\n".join(
        (
            f"- {timestamp_summary(accel_raw, accel_rate, 'accel_g')}",
            f"- {timestamp_summary(gyro_raw, gyro_rate, 'gyro_dps')}",
        )
    )

    detection_details = (
        f"- Start source: **{start_source}**\n"
        f"- Movement onset: **{movement_start_s:.4f} s**\n"
        f"- Gyro activity peak: **{detection.peak_time_s:.4f} s**\n"
        f"- Peak envelope: **{detection.peak_activity_dps:.4f} deg/s**\n"
        f"- High threshold: **{detection.high_threshold_dps:.4f} deg/s**\n"
        f"- Baseline median: **{detection.baseline_median_dps:.4f} deg/s**\n"
        f"- Baseline robust sigma: **{detection.baseline_sigma_dps:.4f} deg/s**"
    )

    header = pn.pane.Markdown(
        f"""
# Moog Gen 3 MMS+ motion and vibration report

**Input:** `{args.csv_file}`

## Movement-start detection

{detection_details}

The dashed vertical line on the plots marks the estimated onset of the large
gyroscope event produced when the platform begins moving to its neutral
position. The detector marks the event onset, not its maximum.

## Analysis intervals

- Stationary reference: **{stationary_start_s:.4f}–{stationary_stop_s:.4f} s**
- Moving interval: **{movement_start_s:.4f}–{movement_end_s:.4f} s**
- Stationary guard before movement: **{args.stationary_guard:.3f} s**

## Sampling checks

{timestamp_checks}

- Lowest Nyquist frequency: **{minimum_nyquist:.3f} Hz**
- Motion-verification low-pass: **0–{args.motion_cutoff:g} Hz**
- Vibration analysis band: **{args.vibration_low:g}–{args.vibration_high:g} Hz**
"""
    )

    aliasing_warning = pn.pane.Alert(
        (
            "Digital filtering only prevents the report from displaying "
            "out-of-band sampled content. It cannot remove vibration that "
            "was already aliased inside the MMS+ before recording. At a "
            "200 Hz sample rate, frequencies above 100 Hz can fold into the "
            "measured band. The MMS+ internal accelerometer and gyroscope "
            "bandwidth/anti-alias settings must therefore be configured with "
            "margin below Nyquist."
        ),
        alert_type="warning",
    )

    motion_note = pn.pane.Alert(
        (
            "The integrated gyroscope angle is intended to verify short "
            "rotational movements and their direction. It is bias-corrected "
            "from the pre-movement stationary interval, but long-duration "
            "integration will still drift. Accelerometer double integration "
            "is intentionally not used as a position measurement."
        ),
        alert_type="info",
    )

    report = pn.Column(
        header,
        aliasing_warning,
        pn.pane.Markdown("## Start-marker detection"),
        detection_plot(detection_data, detection),
        pn.pane.Markdown("## Raw sensor measurements"),
        time_plot(
            accel,
            "Raw accelerometer",
            "Acceleration (g)",
            movement_start_s,
        ),
        time_plot(
            gyro,
            "Raw gyroscope",
            "Angular velocity (deg/s)",
            movement_start_s,
        ),
        pn.pane.Markdown("## Movement verification"),
        motion_note,
        time_plot(
            accel_motion,
            f"Low-frequency accelerometer motion (0–{args.motion_cutoff:g} Hz)",
            "Acceleration (g)",
            movement_start_s,
        ),
        time_plot(
            gyro_motion,
            f"Low-frequency gyroscope motion (0–{args.motion_cutoff:g} Hz)",
            "Angular velocity (deg/s)",
            movement_start_s,
        ),
        time_plot(
            integrated_gyro,
            "Bias-corrected integrated gyroscope angle",
            "Approximate angular displacement (degrees)",
            movement_start_s,
            columns=(
                "x_angle_deg",
                "y_angle_deg",
                "z_angle_deg",
            ),
        ),
        pn.pane.Markdown("## Vibration time histories"),
        time_plot(
            accel_vibration,
            (
                "Accelerometer vibration "
                f"({args.vibration_low:g}–{args.vibration_high:g} Hz)"
            ),
            "Acceleration (g)",
            movement_start_s,
        ),
        time_plot(
            gyro_vibration,
            (
                "Gyroscope vibration "
                f"({args.vibration_low:g}–{args.vibration_high:g} Hz)"
            ),
            "Angular velocity (deg/s)",
            movement_start_s,
        ),
        pn.pane.Markdown("## Stationary versus moving PSD"),
        psd_plot(
            accel_psd,
            "Accelerometer power spectral density",
            "PSD (g²/Hz)",
            args.vibration_high,
        ),
        psd_plot(
            gyro_psd,
            "Gyroscope power spectral density",
            "PSD ((deg/s)²/Hz)",
            args.vibration_high,
        ),
        pn.pane.Markdown("## Vibration metrics"),
        pn.pane.DataFrame(
            metrics.round(6),
            index=False,
            sizing_mode="stretch_width",
        ),
        sizing_mode="stretch_width",
    )

    report.save(
        output_file,
        embed=True,
        resources="cdn",
        title="Moog Gen 3 MMS+ report",
    )

    resolved_output = output_file.resolve()

    print(f"Detected movement start: {movement_start_s:.6f} s")
    print(f"Movement-start source: {start_source}")
    print(f"Stationary interval: {stationary_start_s:.6f}:{stationary_stop_s:.6f} s")
    print(f"Moving interval: {movement_start_s:.6f}:{movement_end_s:.6f} s")
    print(f"HTML report: {resolved_output}")

    if args.metrics_output is not None:
        print(f"Metrics CSV: {args.metrics_output.resolve()}")

    if not args.no_browser:
        webbrowser.open(resolved_output.as_uri())

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
'''

path = Path("/mnt/data/moog_mms_analysis.py")
path.write_text(script)

# Verify that the script parses and compiles.
import py_compile
py_compile.compile(str(path), doraise=True)

print(f"Created and syntax-checked: {path}")

