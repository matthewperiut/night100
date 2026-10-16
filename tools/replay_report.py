#!/usr/bin/env python3
"""Report completed game-frame telemetry for a numbered capture or replay.

Usage: python3 tools/replay_report.py logs/replays/session-000002
       python3 tools/replay_report.py SESSION/replays/RUN --output report.json

GPU rows contain delayed query work observed during an interval. They are not
measurements of that frame's GPU latency and cannot identify a GPU-caused drop.

frames.csv schema 2+ (a frame_schema column; 3 also fills gpu_ms on Switch) adds the zGameLoop phase split:
<phase>_ms wall time per phase and <phase>_cpu_ms producer CPU for update,
render and present. Schema-1 files have no phase section.
"""
import argparse
from collections import Counter
import csv
import json
import math
from pathlib import Path
import statistics
import sys


REQUIRED = {"frame", "wall_ms", "cpu_ms", "process_cpu_ms"}
GPU_NOTE = ("Delayed GPU query work observed per completed-frame interval; "
            "query results can arrive several frames late. These values are not "
            "exact per-frame GPU latency and do not attribute wall/CPU drops to the GPU.")


def number(value):
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def frame_id(value):
    try:
        result = int(value)
    except (TypeError, ValueError):
        return None
    return result if result >= 0 else None


def percentile(values, quantile):
    """Nearest rank, including sensible one-interval results."""
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * quantile) - 1)]


def distribution(values, wall=False):
    result = {"mean": statistics.fmean(values) if values else None,
              "p95": percentile(values, .95), "p99": percentile(values, .99),
              "max": max(values) if values else None}
    if wall:
        result["p50"] = percentile(values, .5)
    return result


PHASES = ("other", "post_present", "pad", "update", "post_update", "render_prep", "render",
          "render_end", "camera_end", "fx_end", "wait", "present")
CPU_PHASES = ("update", "render", "present")
# Producer work that a decoupled renderer could move off the game thread, and
# the simulation work that must stay on it.
GROUPS = {"update_side": ("post_present", "pad", "update", "post_update"),
          "render_side": ("render_prep", "render", "render_end", "camera_end", "fx_end")}
PHASE_NOTE = ("Wall milliseconds between retail zGameLoop call boundaries; present includes the "
              "vertical-blank/display wait (present_ms minus present_cpu_ms). Columns are per completed frame.")


def phase_columns(fieldnames):
    columns = [f"{name}_ms" for name in PHASES] + [f"{name}_cpu_ms" for name in CPU_PHASES]
    return [column for column in columns if column in (fieldnames or ())]


def phase_summary(valid, columns):
    """Means/p95 per phase, plus the same split for the faster and slower half by cpu_ms."""
    if not columns:
        return None
    rows = [row for row in valid if row.get("phases")]
    if not rows:
        return {"interpretation": PHASE_NOTE, "intervals": 0}

    def stats(subset):
        result = {}
        for column in columns:
            values = [row["phases"][column] for row in subset if row["phases"].get(column) is not None]
            if values:
                result[column] = {"mean": statistics.fmean(values), "p95": percentile(values, .95)}
        for group, members in GROUPS.items():
            values = [sum(row["phases"].get(f"{name}_ms") or 0 for name in members) for row in subset]
            if any(f"{name}_ms" in columns for name in members):
                result[f"{group}_ms"] = {"mean": statistics.fmean(values), "p95": percentile(values, .95)}
        return result

    ordered = sorted(rows, key=lambda row: row["cpu_ms"])
    half = len(ordered) // 2
    return {"interpretation": PHASE_NOTE, "intervals": len(rows), "all": stats(rows),
            "fast_half_by_cpu": stats(ordered[:half]) if half else None,
            "slow_half_by_cpu": stats(ordered[half:]) if half else None,
            "median_cpu_ms": percentile([row["cpu_ms"] for row in rows], .5)}


def summarize(rows, columns=()):
    """Summarize DictReader-style rows; timestamps are relative CSV wall sums."""
    valid, excluded = [], Counter()
    elapsed_ms, timestamps_complete, total, invalid_gpu = 0.0, True, 0, 0
    last_frame = None
    for line, row in enumerate(rows, 2):
        total += 1
        wall, cpu, process = (number(row.get(key)) for key in ("wall_ms", "cpu_ms", "process_cpu_ms"))
        start_ms = elapsed_ms
        if wall is not None and wall > 0:
            elapsed_ms += wall
        elif wall is None or wall < 0:
            timestamps_complete = False
        identifier = frame_id(row.get("frame"))
        reason = None
        flag = row.get("timing_valid")
        if flag is not None and str(flag).strip().lower() not in ("1", "true", "yes"):
            reason = "timing_valid_false"
        elif wall is None or wall < 0:
            reason = "invalid_wall_ms"
        elif wall == 0:
            reason = "zero_interval"
        elif identifier is None:
            reason = "invalid_frame_id"
        elif last_frame is not None and identifier <= last_frame:
            reason = "non_increasing_frame_id"
        elif cpu is None or cpu < 0:
            reason = "invalid_cpu_timing"
        elif str(row.get("process_cpu_ms", "")).strip() and (process is None or process < 0):
            reason = "invalid_cpu_timing"
        if identifier is not None:
            last_frame = identifier if last_frame is None else max(last_frame, identifier)
        if reason:
            excluded[reason] += 1
            continue
        gpu_text = row.get("gpu_ms")
        gpu = number(gpu_text)
        if gpu is not None and gpu < 0:
            gpu = None
        if gpu is None and gpu_text is not None and str(gpu_text).strip():
            invalid_gpu += 1
        interval = {"frame": identifier, "csv_line": line,
                    "interval_start_seconds": start_ms / 1000,
                    "interval_end_seconds": elapsed_ms / 1000,
                    "wall_ms": wall, "cpu_ms": cpu, "process_cpu_ms": process,
                    "gpu_query_work_observed_ms": gpu}
        if row.get("clip_frame") not in (None, ""):
            interval["clip_frame"] = frame_id(row["clip_frame"])
        if columns:
            phases = {column: number(row.get(column)) for column in columns}
            if any(value is not None for value in phases.values()):
                interval["phases"] = phases
        valid.append(interval)
    wall = [row["wall_ms"] for row in valid]
    cpu = [row["cpu_ms"] for row in valid]
    process = [row["process_cpu_ms"] for row in valid if row["process_cpu_ms"] is not None]
    observations = [row for row in valid if row["gpu_query_work_observed_ms"] is not None]
    gpu = [row["gpu_query_work_observed_ms"] for row in observations]
    wall_sum = math.fsum(wall)
    phases = phase_summary(valid, list(columns))
    for row in valid:
        row.pop("phases", None)
    result = {
        "rows": total,
        "valid_completed_intervals": len(valid),
        "excluded_intervals": sum(excluded.values()),
        "exclusion_reasons": dict(sorted(excluded.items())),
        "valid_interval_seconds": wall_sum / 1000,
        "game_fps": len(valid) * 1000 / wall_sum if wall_sum else None,
        "fps_basis": "Valid completed game intervals divided by their summed wall time; not window redraw FPS.",
        "wall_ms": distribution(wall, wall=True),
        "cpu_ms": distribution(cpu),
        "process_cpu_ms": distribution(process),
        "process_cpu_available_intervals": len(process),
        "over_20_ms": sum(value > 20 for value in wall),
        "over_33_333_ms": sum(value > 1000 / 30 for value in wall),
        "timestamps": {
            "basis": "Relative to first CSV interval; cumulative finite positive wall_ms, including excluded positive intervals.",
            "complete": timestamps_complete,
            "covered_seconds": elapsed_ms / 1000,
        },
        "gpu_delayed_query_work": {
            "interpretation": GPU_NOTE,
            "observed_intervals": len(gpu),
            "blank_or_unavailable_intervals": len(valid) - len(gpu),
            "invalid_observations": invalid_gpu,
            "total_observed_ms": math.fsum(gpu) if gpu else None,
            "mean_ms_per_observed_interval": statistics.fmean(gpu) if gpu else None,
            "mean_ms_per_valid_completed_interval": math.fsum(gpu) / len(valid) if gpu else None,
            "max_observed_interval_ms": max(gpu) if gpu else None,
            "largest_observation_intervals": sorted(observations, key=lambda row: row["gpu_query_work_observed_ms"], reverse=True)[:10],
        },
        "worst_wall_intervals": sorted(valid, key=lambda row: row["wall_ms"], reverse=True)[:10],
        "worst_cpu_intervals": sorted(valid, key=lambda row: row["cpu_ms"], reverse=True)[:10],
    }
    if phases is not None:
        result["frame_phases"] = phases
    return result


def report(path):
    path = Path(path)
    csv_path = path / "frames.csv" if path.is_dir() else path
    with csv_path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        missing = REQUIRED - set(reader.fieldnames or ())
        if missing:
            raise ValueError("Missing telemetry columns: " + ", ".join(sorted(missing)))
        columns = phase_columns(reader.fieldnames)
        result = summarize(reader, columns)
    frame_schema = 1
    if "frame_schema" in (reader.fieldnames or ()):
        with csv_path.open(newline="") as stream:
            first = next(csv.DictReader(stream), {})
        frame_schema = int(first.get("frame_schema") or 2)
    return {"schema": 2, "frame_schema": frame_schema, "source": str(csv_path), **result}


def phase_lines(report):
    """Human-readable update/render split for stderr."""
    phases = report.get("frame_phases")
    if not phases or not phases.get("intervals"):
        return []
    lines = [f"frame phases over {phases['intervals']} intervals (median cpu {phases['median_cpu_ms']:.2f} ms):"]
    for label, key in (("all", "all"), ("fast half", "fast_half_by_cpu"), ("slow half", "slow_half_by_cpu")):
        split = phases.get(key) or {}
        cells = []
        for column in ("update_ms", "render_ms", "update_side_ms", "render_side_ms", "present_ms",
                       "update_cpu_ms", "render_cpu_ms", "present_cpu_ms"):
            if column in split:
                cells.append(f"{column} {split[column]['mean']:.2f}/{split[column]['p95']:.2f}")
        lines.append(f"  {label:9s} mean/p95: " + ", ".join(cells))
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", type=Path, help="Numbered session/replay directory or frames.csv")
    parser.add_argument("--output", "-o", type=Path, help="Also write this JSON file")
    args = parser.parse_args(argv)
    try:
        result = report(args.capture)
        text = json.dumps(result, indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(text)
    except (OSError, ValueError) as error:
        parser.exit(2, f"replay_report: {error}\n")
    print(text, end="")
    for line in phase_lines(result):
        print(line, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
