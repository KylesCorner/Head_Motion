#!/usr/bin/env python3

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from scipy.interpolate import interp1d
from scipy.signal import butter, sosfiltfilt, find_peaks

import holoviews as hv
import hvplot.pandas
import panel as pn


hv.extension("bokeh")
pn.extension()


# ============================================================================
# Filters
# ============================================================================

def bandpass_filter(data, fs, low_hz, high_hz, order=4):
    """Zero-phase Butterworth band-pass filter."""

    nyquist = fs / 2.0

    if low_hz <= 0:
        raise ValueError("Low cutoff must be > 0 Hz")

    if high_hz >= nyquist:
        print(
            f"Warning: requested high cutoff {high_hz:.2f} Hz is above "
            f"Nyquist ({nyquist:.2f} Hz)."
        )

        high_hz = nyquist * 0.95

        print(f"Using {high_hz:.2f} Hz instead.")

    if low_hz >= high_hz:
        raise ValueError(
            f"Invalid filter range {low_hz:.2f}-{high_hz:.2f} Hz "
            f"for sample rate {fs:.2f} Hz"
        )

    sos = butter(
        order,
        [low_hz, high_hz],
        btype="bandpass",
        fs=fs,
        output="sos",
    )

    return sosfiltfilt(sos, data)


# ============================================================================
# CSV
# ============================================================================

def load_accelerometer(csv_path):
    df = pd.read_csv(csv_path)

    required = {
        "elapsed_ms",
        "sensor",
        "x",
        "y",
        "z",
    }

    missing = required - set(df.columns)

    if missing:
        raise ValueError(
            f"Missing required columns: {sorted(missing)}"
        )

    accel = df[df["sensor"] == "accel_g"].copy()

    if accel.empty:
        raise ValueError(
            "CSV does not contain any sensor == 'accel_g' rows"
        )

    accel = accel.sort_values("elapsed_ms")
    accel = accel.drop_duplicates(
        subset="elapsed_ms",
        keep="first",
    )
    accel = accel.reset_index(drop=True)

    accel["time_s"] = accel["elapsed_ms"] / 1000.0

    # Convert to time relative to first accelerometer sample.
    accel["time_s"] -= accel["time_s"].iloc[0]

    return accel


# ============================================================================
# Sampling
# ============================================================================

def estimate_sample_rate(time_s):
    dt = np.diff(time_s)

    dt = dt[
        np.isfinite(dt) &
        (dt > 0)
    ]

    if len(dt) == 0:
        raise ValueError(
            "Could not determine accelerometer sample rate"
        )

    median_dt = np.median(dt)

    fs = 1.0 / median_dt

    return fs


def resample_uniform(accel, fs):
    """
    Interpolate X/Y/Z onto a uniform time grid.

    scipy digital filters assume samples are evenly spaced.
    """

    t = accel["time_s"].to_numpy()

    dt = 1.0 / fs

    t_uniform = np.arange(
        t[0],
        t[-1],
        dt,
    )

    output = {
        "time_s": t_uniform,
    }

    for axis in ("x", "y", "z"):
        f = interp1d(
            t,
            accel[axis].to_numpy(),
            kind="linear",
            bounds_error=False,
            fill_value="extrapolate",
        )

        output[axis] = f(t_uniform)

    return pd.DataFrame(output)


# ============================================================================
# Vector calculations
# ============================================================================

def calculate_acceleration_vectors(df):
    x = df["x"].to_numpy()
    y = df["y"].to_numpy()
    z = df["z"].to_numpy()

    df = df.copy()

    df["accel_mag_g"] = np.sqrt(
        x * x +
        y * y +
        z * z
    )

    return df


# ============================================================================
# Event detection
# ============================================================================

def detect_wrist_events(
    signal,
    fs,
    min_spacing_s=0.18,
    prominence=None,
):
    """
    Detect rhythmic wrist-motion events.

    We detect peaks in the absolute filtered acceleration so motion in
    either direction contributes an event.
    """

    motion = np.abs(signal)

    if prominence is None:
        prominence = 0.50 * np.std(motion)

    distance = max(
        1,
        int(min_spacing_s * fs),
    )

    peaks, properties = find_peaks(
        motion,
        distance=distance,
        prominence=prominence,
    )

    return peaks, properties


def detect_pick_events(
    signal,
    fs,
    min_spacing_s=0.030,
    prominence=None,
):
    """
    Detect candidate pick/string transients from the higher-frequency band.
    """

    envelope = np.abs(signal)

    if prominence is None:
        prominence = 1.5 * np.std(envelope)

    distance = max(
        1,
        int(min_spacing_s * fs),
    )

    peaks, properties = find_peaks(
        envelope,
        distance=distance,
        prominence=prominence,
    )

    return peaks, properties, envelope


# ============================================================================
# Tempo
# ============================================================================

def calculate_local_tempo(
    event_times,
    total_time,
    window_s=6.0,
    step_s=0.25,
):
    """
    Calculate local wrist-event rate using a sliding window.

    NOTE:
        This is motion events/min, not necessarily the musical BPM.
        Eighth-note wrist motion can produce approximately 2x musical BPM.
    """

    if total_time <= window_s:
        raise ValueError(
            "Recording is shorter than the tempo window"
        )

    centers = np.arange(
        window_s / 2.0,
        total_time - window_s / 2.0,
        step_s,
    )

    tempo = np.full(
        len(centers),
        np.nan,
        dtype=float,
    )

    half_window = window_s / 2.0

    for i, center in enumerate(centers):
        start = center - half_window
        end = center + half_window

        events = event_times[
            (event_times >= start) &
            (event_times <= end)
        ]

        if len(events) < 3:
            continue

        intervals = np.diff(events)

        # Reject obviously unreasonable detections.
        intervals = intervals[
            (intervals >= 0.10) &
            (intervals <= 2.0)
        ]

        if len(intervals) == 0:
            continue

        median_interval = np.median(intervals)

        tempo[i] = 60.0 / median_interval

    return pd.DataFrame({
        "time_s": centers,
        "tempo_epm": tempo,
    })


# ============================================================================
# Display decimation
# ============================================================================

def decimate_for_display(df, max_points):
    """
    Reduce only the number of points written into the HTML.

    All filtering and event detection is still performed at full resolution.
    """

    if len(df) <= max_points:
        return df

    stride = int(
        np.ceil(len(df) / max_points)
    )

    print(
        f"Display decimation: {len(df):,} -> "
        f"{len(df.iloc[::stride]):,} points "
        f"(stride={stride})"
    )

    return df.iloc[::stride].copy()


# ============================================================================
# Event output
# ============================================================================

def build_event_table(
    data,
    wrist_signal,
    pick_signal,
    wrist_peaks,
    pick_peaks,
):
    events = []

    time_s = data["time_s"].to_numpy()

    for idx in wrist_peaks:
        events.append({
            "time_s": time_s[idx],
            "event": "wrist",
            "amplitude_g": abs(wrist_signal[idx]),
        })

    for idx in pick_peaks:
        events.append({
            "time_s": time_s[idx],
            "event": "pick_candidate",
            "amplitude_g": abs(pick_signal[idx]),
        })

    events = pd.DataFrame(events)

    if not events.empty:
        events = events.sort_values(
            "time_s"
        ).reset_index(drop=True)

    return events


# ============================================================================
# hvPlot dashboard
# ============================================================================

def create_dashboard(
    data,
    wrist_signal,
    wrist_peaks,
    pick_signal,
    pick_envelope,
    pick_peaks,
    tempo_df,
    fs,
    args,
):
    t = data["time_s"].to_numpy()

    # ----------------------------------------------------------------------
    # Build plotting DataFrame
    # ----------------------------------------------------------------------

    plot_df = pd.DataFrame({
        "time_s": t,
        "accel_x_g": data["x"],
        "accel_y_g": data["y"],
        "accel_z_g": data["z"],
        "accel_mag_g": data["accel_mag_g"],
        "wrist_filtered_g": wrist_signal,
        "pick_filtered_g": pick_signal,
        "pick_envelope_g": pick_envelope,
    })

    display_df = decimate_for_display(
        plot_df,
        args.max_display_points,
    )

    # ----------------------------------------------------------------------
    # Wrist event DataFrame
    # ----------------------------------------------------------------------

    wrist_events = pd.DataFrame({
        "time_s": t[wrist_peaks],
        "wrist_filtered_g": wrist_signal[wrist_peaks],
        "magnitude_g": np.abs(wrist_signal[wrist_peaks]),
        "event": "wrist",
    })

    # ----------------------------------------------------------------------
    # Pick event DataFrame
    # ----------------------------------------------------------------------

    pick_events = pd.DataFrame({
        "time_s": t[pick_peaks],
        "pick_filtered_g": pick_signal[pick_peaks],
        "pick_envelope_g": pick_envelope[pick_peaks],
        "event": "pick_candidate",
    })

    common = dict(
        x="time_s",
        responsive=True,
        height=300,
        tools=[
            "hover",
            "pan",
            "wheel_zoom",
            "box_zoom",
            "reset",
            "save",
        ],
    )

    # ======================================================================
    # Plot 1: XYZ axes
    # ======================================================================

    xyz_plot = display_df.hvplot.line(
        y=[
            "accel_x_g",
            "accel_y_g",
            "accel_z_g",
        ],
        xlabel="Time (s)",
        ylabel="Acceleration (g)",
        title="Raw Accelerometer Axes",
        legend="top_right",
        **common,
    )

    # ======================================================================
    # Plot 2: vector magnitude
    # ======================================================================

    magnitude_plot = display_df.hvplot.line(
        y="accel_mag_g",
        xlabel="Time (s)",
        ylabel="Acceleration magnitude (g)",
        title="Acceleration Vector Magnitude",
        **common,
    )

    # ======================================================================
    # Plot 3: wrist/rhythm band
    # ======================================================================

    wrist_line = display_df.hvplot.line(
        y="wrist_filtered_g",
        xlabel="Time (s)",
        ylabel="Filtered acceleration (g)",
        title=(
            f"Wrist Motion Band "
            f"({args.wrist_low:g}-{args.wrist_high:g} Hz)"
        ),
        **common,
    )

    wrist_points = wrist_events.hvplot.scatter(
        x="time_s",
        y="wrist_filtered_g",
        hover_cols=[
            "time_s",
            "magnitude_g",
        ],
        size=7,
        marker="circle",
    )

    wrist_plot = wrist_line * wrist_points

    # ======================================================================
    # Plot 4: pick frequency band
    # ======================================================================

    pick_line = display_df.hvplot.line(
        y="pick_filtered_g",
        xlabel="Time (s)",
        ylabel="Filtered acceleration (g)",
        title=(
            f"Candidate Pick / String Transient Band "
            f"({args.pick_low:g}-{args.pick_high:g} Hz)"
        ),
        **common,
    )

    pick_points = pick_events.hvplot.scatter(
        x="time_s",
        y="pick_filtered_g",
        hover_cols=[
            "time_s",
            "pick_envelope_g",
        ],
        size=6,
        marker="triangle",
    )

    pick_plot = pick_line * pick_points

    # ======================================================================
    # Plot 5: rectified pick envelope
    # ======================================================================

    envelope_line = display_df.hvplot.line(
        y="pick_envelope_g",
        xlabel="Time (s)",
        ylabel="Transient magnitude (g)",
        title="Pick / String Transient Envelope",
        **common,
    )

    envelope_points = pick_events.hvplot.scatter(
        x="time_s",
        y="pick_envelope_g",
        hover_cols=[
            "time_s",
            "pick_envelope_g",
        ],
        size=6,
    )

    envelope_plot = envelope_line * envelope_points

    # ======================================================================
    # Plot 6: local tempo
    # ======================================================================

    tempo_plot = tempo_df.hvplot.line(
        x="time_s",
        y="tempo_epm",
        xlabel="Time (s)",
        ylabel="Motion events / minute",
        title=(
            f"Local Wrist Motion Tempo "
            f"({args.tempo_window:g} s window)"
        ),
        responsive=True,
        height=300,
        tools=[
            "hover",
            "pan",
            "wheel_zoom",
            "box_zoom",
            "reset",
            "save",
        ],
    )

    # ======================================================================
    # Layout
    # ======================================================================

    # Because every plot uses a time_s x-dimension, HoloViews can share
    # the Bokeh x-range. Zooming/panning one plot therefore updates the
    # others.
    plots = (
        xyz_plot
        + magnitude_plot
        + wrist_plot
        + pick_plot
        + envelope_plot
        + tempo_plot
    ).cols(1)

    plots = plots.opts(
        shared_axes=True,
    )

    # ----------------------------------------------------------------------
    # Summary
    # ----------------------------------------------------------------------

    wrist_count = len(wrist_peaks)
    pick_count = len(pick_peaks)

    duration = t[-1]

    wrist_times = t[wrist_peaks]

    if len(wrist_times) >= 2:
        intervals = np.diff(wrist_times)
        median_event_rate = 60.0 / np.median(intervals)
        tempo_string = f"{median_event_rate:.2f} events/min"
    else:
        tempo_string = "Not enough events"

    summary = pn.pane.Markdown(
        f"""
# Guitar Wrist IMU Rhythm Analysis

**Recording**

- Duration: `{duration:.2f} s`
- Accelerometer sample rate: `{fs:.2f} Hz`
- Accelerometer samples: `{len(data):,}`

**Detection**

- Wrist events: `{wrist_count:,}`
- Candidate pick/string events: `{pick_count:,}`
- Median wrist event rate: `{tempo_string}`

**Filter bands**

- Wrist motion: `{args.wrist_low:g}–{args.wrist_high:g} Hz`
- Pick/string transients: `{args.pick_low:g}–{args.pick_high:g} Hz`

The wrist value is a **motion event rate**, not automatically the
musical BPM. A player moving on eighth notes, for example, can produce
approximately twice the musical tempo.

Use the **wheel zoom**, **box zoom**, and **pan** tools to inspect short
sections of the performance. The time axes are linked between plots.
"""
    )

    dashboard = pn.Column(
        summary,
        plots,
        sizing_mode="stretch_width",
    )

    return dashboard


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description=(
            "Analyze wrist-mounted guitar IMU acceleration and generate "
            "an interactive standalone hvPlot HTML report."
        )
    )

    parser.add_argument(
        "csv",
        type=Path,
        help="Input IMU CSV file",
    )

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("imu_rhythm_analysis.html"),
        help=(
            "Output HTML file "
            "(default: imu_rhythm_analysis.html)"
        ),
    )

    parser.add_argument(
        "--events-output",
        type=Path,
        default=Path("imu_events.csv"),
        help="Detected event CSV",
    )

    # ----------------------------------------------------------------------
    # Wrist filter
    # ----------------------------------------------------------------------

    parser.add_argument(
        "--wrist-low",
        type=float,
        default=0.5,
        help="Wrist lower cutoff Hz (default: 0.5)",
    )

    parser.add_argument(
        "--wrist-high",
        type=float,
        default=8.0,
        help="Wrist upper cutoff Hz (default: 8)",
    )

    # ----------------------------------------------------------------------
    # Pick filter
    # ----------------------------------------------------------------------

    parser.add_argument(
        "--pick-low",
        type=float,
        default=8.0,
        help="Pick transient lower cutoff Hz (default: 8)",
    )

    parser.add_argument(
        "--pick-high",
        type=float,
        default=40.0,
        help="Pick transient upper cutoff Hz (default: 40)",
    )

    # ----------------------------------------------------------------------
    # Detection
    # ----------------------------------------------------------------------

    parser.add_argument(
        "--wrist-prominence",
        type=float,
        default=None,
        help="Manual wrist peak prominence threshold",
    )

    parser.add_argument(
        "--pick-prominence",
        type=float,
        default=None,
        help="Manual pick peak prominence threshold",
    )

    parser.add_argument(
        "--wrist-spacing",
        type=float,
        default=0.18,
        help=(
            "Minimum wrist-event spacing in seconds "
            "(default: 0.18)"
        ),
    )

    parser.add_argument(
        "--pick-spacing",
        type=float,
        default=0.030,
        help=(
            "Minimum pick-event spacing in seconds "
            "(default: 0.030)"
        ),
    )

    # ----------------------------------------------------------------------
    # Tempo
    # ----------------------------------------------------------------------

    parser.add_argument(
        "--tempo-window",
        type=float,
        default=6.0,
        help="Local tempo window in seconds (default: 6)",
    )

    parser.add_argument(
        "--tempo-step",
        type=float,
        default=0.25,
        help="Local tempo calculation step (default: 0.25)",
    )

    # ----------------------------------------------------------------------
    # HTML rendering
    # ----------------------------------------------------------------------

    parser.add_argument(
        "--max-display-points",
        type=int,
        default=150_000,
        help=(
            "Maximum signal points embedded in HTML. "
            "Processing still uses all samples. "
            "(default: 150000)"
        ),
    )

    parser.add_argument(
        "--cdn",
        action="store_true",
        help=(
            "Use CDN JavaScript resources instead of embedding them "
            "into the HTML. Produces a smaller file but requires internet."
        ),
    )

    args = parser.parse_args()

    # ======================================================================
    # Load data
    # ======================================================================

    print(f"Loading: {args.csv}")

    accel = load_accelerometer(
        args.csv
    )

    fs = estimate_sample_rate(
        accel["time_s"].to_numpy()
    )

    print(
        f"Estimated sample rate: {fs:.3f} Hz"
    )

    # ======================================================================
    # Uniform resampling
    # ======================================================================

    print("Resampling to uniform timebase...")

    data = resample_uniform(
        accel,
        fs,
    )

    data = calculate_acceleration_vectors(
        data
    )

    t = data["time_s"].to_numpy()

    print(
        f"Duration: {t[-1]:.2f} s"
    )

    print(
        f"Samples: {len(data):,}"
    )

    # ======================================================================
    # Wrist filter
    # ======================================================================

    print(
        f"Filtering wrist band: "
        f"{args.wrist_low:g}-{args.wrist_high:g} Hz"
    )

    wrist_signal = bandpass_filter(
        data["accel_mag_g"].to_numpy(),
        fs,
        args.wrist_low,
        args.wrist_high,
    )

    wrist_peaks, _ = detect_wrist_events(
        wrist_signal,
        fs,
        min_spacing_s=args.wrist_spacing,
        prominence=args.wrist_prominence,
    )

    print(
        f"Wrist events: {len(wrist_peaks):,}"
    )

    # ======================================================================
    # Pick filter
    # ======================================================================

    print(
        f"Filtering pick band: "
        f"{args.pick_low:g}-{args.pick_high:g} Hz"
    )

    pick_signal = bandpass_filter(
        data["accel_mag_g"].to_numpy(),
        fs,
        args.pick_low,
        args.pick_high,
    )

    pick_peaks, _, pick_envelope = detect_pick_events(
        pick_signal,
        fs,
        min_spacing_s=args.pick_spacing,
        prominence=args.pick_prominence,
    )

    print(
        f"Candidate pick/string events: "
        f"{len(pick_peaks):,}"
    )

    # ======================================================================
    # Tempo
    # ======================================================================

    wrist_times = t[wrist_peaks]

    tempo_df = calculate_local_tempo(
        wrist_times,
        total_time=t[-1],
        window_s=args.tempo_window,
        step_s=args.tempo_step,
    )

    # ======================================================================
    # Events CSV
    # ======================================================================

    events = build_event_table(
        data,
        wrist_signal,
        pick_signal,
        wrist_peaks,
        pick_peaks,
    )

    events.to_csv(
        args.events_output,
        index=False,
    )

    print(
        f"Events written: {args.events_output}"
    )

    # ======================================================================
    # Interactive report
    # ======================================================================

    print("Building interactive hvPlot report...")

    dashboard = create_dashboard(
        data=data,
        wrist_signal=wrist_signal,
        wrist_peaks=wrist_peaks,
        pick_signal=pick_signal,
        pick_envelope=pick_envelope,
        pick_peaks=pick_peaks,
        tempo_df=tempo_df,
        fs=fs,
        args=args,
    )

    resources = (
        "cdn"
        if args.cdn
        else "inline"
    )

    dashboard.save(
        args.output,
        resources=resources,
        title="Guitar Wrist IMU Rhythm Analysis",
    )

    print()
    print(f"HTML report: {args.output}")
    print(f"Event CSV:   {args.events_output}")

    if resources == "inline":
        print(
            "Bokeh/Panel resources are embedded in the HTML; "
            "internet access is not required."
        )


if __name__ == "__main__":
    main()
