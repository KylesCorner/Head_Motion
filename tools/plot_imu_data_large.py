#!/usr/bin/env python3
"""
Plot every raw HeadMotion IMU sample from a very large CSV without loading the
entire file into RAM.

Expected columns:
    epoch_ms,sensor,x,y,z

Expected sensor values:
    accel_g
    gyro_dps

Output:
    accel_g.html
    gyro_dps.html

Important:
    This script does NOT average, downsample, smooth, decimate, or discard rows.
    Dask reads the CSV out-of-core, and Datashader rasterizes every original line
    segment into the output pixels. This is the practical way to visualize the
    complete raw dataset without sending millions of browser glyphs to Bokeh.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import dask.dataframe as dd
import datashader as ds
import holoviews as hv
import hvplot.dask  # noqa: F401 - registers the Dask .hvplot accessor


SENSORS = {
    "accel_g": {
        "title": "Raw acceleration",
        "ylabel": "Acceleration (g)",
        "filename": "accel_g.html",
    },
    "gyro_dps": {
        "title": "Raw angular velocity",
        "ylabel": "Angular velocity (deg/s)",
        "filename": "gyro_dps.html",
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot every raw accel_g and gyro_dps sample from a large CSV."
    )
    parser.add_argument("csv", type=Path, help="Path to imu.csv")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("imu_plots"),
        help="Output directory (default: imu_plots)",
    )
    parser.add_argument(
        "--blocksize",
        default="64MB",
        help="Dask CSV partition size, such as 32MB or 128MB (default: 64MB)",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=3840,
        help="Raster width in pixels (default: 3840)",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=1200,
        help="Raster height in pixels (default: 1200)",
    )
    parser.add_argument(
        "--absolute-time",
        action="store_true",
        help="Plot UTC timestamps instead of seconds since the first IMU sample",
    )
    parser.add_argument(
        "--line-width",
        type=float,
        default=0.0,
        help=(
            "Datashader antialiasing width. Zero gives the least-altered raw "
            "pixel rendering (default: 0)."
        ),
    )
    parser.add_argument(
        "--cdn",
        action="store_true",
        help="Load Bokeh JavaScript from a CDN instead of embedding it",
    )

    args = parser.parse_args()

    if args.width < 500:
        parser.error("--width must be at least 500")
    if args.height < 300:
        parser.error("--height must be at least 300")
    if args.line_width < 0:
        parser.error("--line-width cannot be negative")

    return args


def read_imu_csv(csv_path: Path, blocksize: str) -> dd.DataFrame:
    """
    Build a lazy Dask DataFrame.

    float64 is intentional: the script does not reduce the numeric precision of
    the values parsed from the CSV.
    """
    try:
        frame = dd.read_csv(
            csv_path,
            blocksize=blocksize,
            usecols=["epoch_ms", "sensor", "x", "y", "z"],
            dtype={
                "epoch_ms": "float64",
                "sensor": "object",
                "x": "float64",
                "y": "float64",
                "z": "float64",
            },
            assume_missing=True,
        )
    except ValueError as exc:
        raise RuntimeError(
            "CSV must contain the columns epoch_ms,sensor,x,y,z"
        ) from exc

    # Filtering is lazy. No CSV partitions are loaded here.
    return frame[frame["sensor"].isin(list(SENSORS))]


def make_raw_plot(
    sensor_frame: dd.DataFrame,
    sensor_name: str,
    time_column: str,
    xlabel: str,
    width: int,
    height: int,
    line_width: float,
):
    config = SENSORS[sensor_name]

    # Do not pass y=["x", "y", "z"] here. With a Dask DataFrame, that wide-form
    # path makes HoloViews attempt row-based DataFrame.iloc slicing, which Dask
    # intentionally does not support.
    #
    # Rasterize each raw axis independently first. Each operation therefore sees
    # one Curve backed by one Dask DataFrame. The resulting raster layers are then
    # overlaid. Every original line segment is still processed by Datashader.
    axis_colors = {
        "x": "#d62728",
        "y": "#2ca02c",
        "z": "#1f77b4",
    }

    layers = []

    for axis in ("x", "y", "z"):
        layer = sensor_frame[[time_column, axis]].hvplot.line(
            x=time_column,
            y=axis,
            label=axis.upper(),
            rasterize=True,
            dynamic=False,
            aggregator=ds.any(),
            cmap=[axis_colors[axis]],
            colorbar=False,
            width=width,
            height=height,
            line_width=line_width,
            tools=["xpan", "xwheel_zoom", "box_zoom", "reset", "save"],
        )
        layers.append(layer)

    plot = layers[0] * layers[1] * layers[2]

    return plot.opts(
        title=config["title"],
        xlabel=xlabel,
        ylabel=config["ylabel"],
        legend_position="top_left",
        show_grid=True,
        width=width,
        height=height,
    )


def main() -> int:
    args = parse_args()
    csv_path = args.csv.expanduser().resolve()

    if not csv_path.is_file():
        print(f"error: file not found: {csv_path}", file=sys.stderr)
        return 2

    args.output_dir.mkdir(parents=True, exist_ok=True)
    hv.extension("bokeh")

    print(f"Opening lazily with Dask: {csv_path}")
    imu = read_imu_csv(csv_path, args.blocksize)

    print("Finding the raw IMU time range...")
    start_ms, end_ms = dd.compute(imu["epoch_ms"].min(), imu["epoch_ms"].max())

    if start_ms is None or end_ms is None:
        print("error: no accel_g or gyro_dps rows found", file=sys.stderr)
        return 1

    start_ms = float(start_ms)
    end_ms = float(end_ms)

    if args.absolute_time:
        # Datashader and Bokeh handle datetime64 values directly.
        imu = imu.assign(
            plot_time=dd.to_datetime(imu["epoch_ms"], unit="ms", utc=True)
        )
        time_column = "plot_time"
        xlabel = "Time (UTC)"
    else:
        imu = imu.assign(
            elapsed_s=(imu["epoch_ms"] - start_ms) / 1000.0
        )
        time_column = "elapsed_s"
        xlabel = "Time since first IMU sample (s)"

    resources = "cdn" if args.cdn else "inline"

    for sensor_name, config in SENSORS.items():
        print(f"Rasterizing every raw {sensor_name} sample...")
        sensor_frame = imu[imu["sensor"] == sensor_name][
            [time_column, "x", "y", "z"]
        ]

        plot = make_raw_plot(
            sensor_frame=sensor_frame,
            sensor_name=sensor_name,
            time_column=time_column,
            xlabel=xlabel,
            width=args.width,
            height=args.height,
            line_width=args.line_width,
        )

        output_path = args.output_dir / config["filename"]
        hv.save(
            plot,
            output_path,
            backend="bokeh",
            resources=resources,
            title=f"HeadMotion {config['title']}",
        )
        print(f"Wrote: {output_path}")

    print("\nDone. No time-bin averaging or downsampling was applied.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
