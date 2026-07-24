#!/usr/bin/env python3

import argparse
import sys
import webbrowser
from pathlib import Path

import hvplot.pandas  # Registers .hvplot on pandas objects
import pandas as pd
import panel as pn


REQUIRED_COLUMNS = {"epoch_ms", "sensor", "x", "y", "z"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot MMS+ accelerometer and gyroscope CSV data using hvPlot."
    )

    parser.add_argument(
        "csv_file",
        type=Path,
        help="Path to the input CSV file.",
    )

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output HTML file. Defaults to <input_name>_plot.html.",
    )

    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="Do not automatically open the generated HTML file.",
    )

    return parser.parse_args()


def load_csv(csv_file: Path) -> pd.DataFrame:
    if not csv_file.is_file():
        raise FileNotFoundError(f"CSV file does not exist: {csv_file}")

    data = pd.read_csv(csv_file)

    missing_columns = REQUIRED_COLUMNS - set(data.columns)
    if missing_columns:
        missing = ", ".join(sorted(missing_columns))
        raise ValueError(f"CSV is missing required columns: {missing}")

    numeric_columns = ["epoch_ms", "x", "y", "z"]

    for column in numeric_columns:
        data[column] = pd.to_numeric(data[column], errors="coerce")

    invalid_rows = data[numeric_columns].isna().any(axis=1).sum()

    if invalid_rows:
        print(
            f"Warning: removing {invalid_rows} rows containing invalid numeric data.",
            file=sys.stderr,
        )
        data = data.dropna(subset=numeric_columns)

    if data.empty:
        raise ValueError("The CSV contains no valid samples.")

    data = data.sort_values("epoch_ms").reset_index(drop=True)

    # Use elapsed time instead of the large absolute epoch timestamp.
    start_epoch_ms = data["epoch_ms"].min()
    data["time_s"] = (data["epoch_ms"] - start_epoch_ms) / 1000.0

    # Also retain a human-readable timestamp for hover information.
    data["timestamp"] = pd.to_datetime(
        data["epoch_ms"],
        unit="ms",
        errors="coerce",
    )

    return data


def create_sensor_plot(
    data: pd.DataFrame,
    sensor_name: str,
    title: str,
    y_label: str,
):
    sensor_data = data[data["sensor"] == sensor_name].copy()

    if sensor_data.empty:
        return pn.pane.Alert(
            f"No samples labeled '{sensor_name}' were found.",
            alert_type="warning",
        )

    return sensor_data.hvplot.line(
        x="time_s",
        y=["x", "y", "z"],
        title=title,
        xlabel="Elapsed time (seconds)",
        ylabel=y_label,
        hover_cols=["timestamp", "epoch_ms"],
        responsive=True,
        height=400,
        line_width=1.5,
        legend="top_right",
        grid=True,
    )


def main() -> int:
    args = parse_arguments()

    output_file = args.output
    if output_file is None:
        output_file = args.csv_file.with_name(
            f"{args.csv_file.stem}_plot.html"
        )

    try:
        data = load_csv(args.csv_file)
    except (FileNotFoundError, ValueError, pd.errors.ParserError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    accel_plot = create_sensor_plot(
        data=data,
        sensor_name="accel_g",
        title="MMS+ Accelerometer",
        y_label="Acceleration (g)",
    )

    gyro_plot = create_sensor_plot(
        data=data,
        sensor_name="gyro_dps",
        title="MMS+ Gyroscope",
        y_label="Angular velocity (degrees/second)",
    )

    dashboard = pn.Column(
        pn.pane.Markdown(f"# MMS+ IMU Data\n\n**Source:** `{args.csv_file}`"),
        accel_plot,
        gyro_plot,
        sizing_mode="stretch_width",
    )

    output_file.parent.mkdir(parents=True, exist_ok=True)

    dashboard.save(
        output_file,
        embed=True,
        resources="cdn",
        title="MMS+ IMU Plot",
    )

    resolved_output = output_file.resolve()
    print(f"Plot written to: {resolved_output}")

    if not args.no_browser:
        webbrowser.open(resolved_output.as_uri())

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
