#!/usr/bin/env python3
"""Compare corrected-reference and candidate playback results, never recordings.

Both directories must be SESSION/replays/RUN results from the same immutable
capture. Matching completed-frame IDs are mandatory before equal warmup removal.
Exit 0: endpoint/image parity; 1: parity drift; 2: invalid/incompatible evidence.
Timing changes are observations, not an automatic performance acceptance rule.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import statistics


CAPTURE_FILES = ("state.bin", "input.inputs", "clock.bin", "metadata.txt")
BASE_FIELDS = {"mode", "state", "ostrich", "scene", "player_valid", "camera_valid"}
PLAYER_FIELDS = {"health", "snacks", "keys", "control", "position_distance", *(f"position_{i}" for i in range(3))}
CAMERA_FIELDS = {f"camera_{i}" for i in range(12)}


def finite(value, name, minimum=None):
    try:
        valid = not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value)
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError(f"{name}: expected finite number")
    if minimum is not None and value < minimum:
        raise ValueError(f"{name}: must be >= {minimum}")
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_verification(path):
    with path.open() as stream:
        data = json.load(stream, object_pairs_hook=unique_object,
                         parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"Nonfinite JSON: {value}")))
    if not isinstance(data, dict) or type(data.get("version")) is not int or data["version"] != 1:
        raise ValueError(f"{path}: unsupported verification schema")
    for key in ("position_tolerance", "camera_tolerance"):
        finite(data.get(key), f"{path}:{key}", 0)
    fields = data.get("fields")
    if not isinstance(fields, dict) or not BASE_FIELDS <= fields.keys():
        raise ValueError(f"{path}: missing required semantic fields")
    for key, field in fields.items():
        if not isinstance(field, dict) or set(field) != {"expected", "actual", "error", "tolerance", "matches"}:
            raise ValueError(f"{path}:{key}: incomplete semantic field")
        for part in ("expected", "actual", "error", "tolerance"):
            finite(field[part], f"{path}:{key}:{part}", 0 if part in ("error", "tolerance") else None)
        if type(field["matches"]) is not bool:
            raise ValueError(f"{path}:{key}: invalid matches flag")
    required = set(BASE_FIELDS)
    for key in ("player_valid", "camera_valid"):
        if fields[key]["actual"] not in (0, 1) or fields[key]["expected"] not in (0, 1):
            raise ValueError(f"{path}:{key}: invalid validity flag")
    # A live player/camera needs real state, even when the original snapshot's
    # expectation lacked it. A sparse result cannot establish native parity.
    if fields["player_valid"]["actual"]:
        required |= PLAYER_FIELDS
    if fields["camera_valid"]["actual"]:
        required |= CAMERA_FIELDS
    if not required <= fields.keys() or not fields.keys() <= BASE_FIELDS | PLAYER_FIELDS | CAMERA_FIELDS:
        raise ValueError(f"{path}: missing or unknown semantic fields")
    for key in BASE_FIELDS | {"health", "snacks", "keys", "control"}:
        if key in fields and any(value != int(value) for value in (fields[key]["actual"], fields[key]["expected"])):
            raise ValueError(f"{path}:{key}: expected integer state")
    if type(data.get("matches")) is not bool or data["matches"] != all(field["matches"] for field in fields.values()):
        raise ValueError(f"{path}: inconsistent overall matches flag")
    return data


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def identity(directory):
    if directory.parent.name != "replays":
        raise ValueError(f"{directory}: expected SESSION/replays/RUN result")
    session = directory.parent.parent
    return {name: sha256(session / name) for name in CAPTURE_FILES}


def expected_frame_ids(directory):
    with (directory.parent.parent / "metadata.txt").open() as stream:
        header = shlex.split(stream.readline())
    if len(header) != 8 or header[:2] != ["NIGHT100_SESSION", "1"] or header[-1] != "1":
        raise ValueError("Incomplete or incompatible source metadata")
    start, end = int(header[-3]), int(header[-2])
    if start < 0 or end <= start:
        raise ValueError("Invalid source capture frame range")
    return list(range(start + 1, end + 1))


def image(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise ValueError(f"{path}: invalid P6 endpoint image")
    width, height = map(int, header.groups())
    if not 0 < width <= 16384 or not 0 < height <= 16384 or len(data) - header.end() != width * height * 3:
        raise ValueError(f"{path}: incomplete endpoint image")
    return {"sha256": hashlib.sha256(data).hexdigest(), "width": width, "height": height}


def read_frames(path):
    rows, last, last_clip = [], None, None
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        names = reader.fieldnames or []
        if len(names) != len(set(names)) or not {"frame", "wall_ms", "cpu_ms", "process_cpu_ms"} <= set(names):
            raise ValueError(f"{path}: missing or duplicate telemetry columns")
        has_clip = "clip_frame" in names
        for line, row in enumerate(reader, 2):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"{path}:{line}: malformed CSV row")
            try:
                frame = int(row["frame"])
                if frame < 0 or last is not None and frame != last + 1:
                    raise ValueError("duplicate, missing or out-of-order frame")
                values = {key: finite(float(row[key]), key, 0) for key in ("wall_ms", "cpu_ms", "process_cpu_ms")}
                if has_clip:
                    clip = int(row["clip_frame"])
                    if clip < 0 or last_clip is not None and clip != last_clip + 1:
                        raise ValueError("duplicate, missing or out-of-order clip frame")
                    last_clip = clip
                else:
                    clip = None
                flag = row.get("timing_valid", "1" if values["wall_ms"] > 0 else "0").strip().lower()
                if flag not in ("0", "1", "true", "false", "yes", "no"):
                    raise ValueError("invalid timing_valid")
                valid = flag in ("1", "true", "yes")
                if valid and values["wall_ms"] == 0:
                    raise ValueError("valid timing has zero interval")
            except (TypeError, ValueError) as error:
                raise ValueError(f"{path}:{line}: {error}") from error
            rows.append({"frame": frame, "clip_frame": clip, "valid": valid, **values})
            last = frame
    if not rows:
        raise ValueError(f"{path}: no completed frames")
    return rows


def distribution(values):
    ordered = sorted(values)
    return {"mean": statistics.fmean(values),
            "p95": ordered[math.ceil(len(values) * .95) - 1],
            "p99": ordered[math.ceil(len(values) * .99) - 1], "max": ordered[-1]}


def semantic(reference, candidate, position_tolerance, camera_tolerance):
    left, right = reference["fields"], candidate["fields"]
    if set(left) != set(right):
        raise ValueError("Incompatible semantic field sets")
    differences = {}
    for name in left:
        if name == "position_distance":
            continue  # distance to the ORIGINAL capture is not an endpoint coordinate
        a, b = left[name]["actual"], right[name]["actual"]
        tolerance = camera_tolerance if name.startswith("camera_") and name != "camera_valid" else 0
        if name.startswith("position_"):
            tolerance = position_tolerance
        differences[name] = {"reference": a, "candidate": b, "error": abs(a - b),
                             "tolerance": tolerance, "matches": abs(a - b) <= tolerance}
    if "position_0" in left:
        distance = math.sqrt(math.fsum((left[f"position_{i}"]["actual"] - right[f"position_{i}"]["actual"]) ** 2 for i in range(3)))
        differences["position_distance"] = {"error": distance, "tolerance": position_tolerance,
                                             "matches": distance <= position_tolerance}
    return {"matches": all(field["matches"] for field in differences.values()), "fields": differences}


def compare(reference, candidate, warmup=60, position_tolerance=.1, camera_tolerance=.02):
    reference, candidate = Path(reference), Path(candidate)
    if type(warmup) is not int or warmup < 0:
        raise ValueError("warmup must be a nonnegative completed-frame count")
    finite(position_tolerance, "position tolerance", 0)
    finite(camera_tolerance, "camera tolerance", 0)
    source = identity(reference)
    if source != identity(candidate):
        raise ValueError("Incompatible source captures: state/input/clock/metadata hashes differ")
    a, b = read_frames(reference / "frames.csv"), read_frames(candidate / "frames.csv")
    identifiers = lambda rows: [(row["frame"], row["clip_frame"], row["valid"]) for row in rows]
    if identifiers(a) != identifiers(b):
        raise ValueError("Mismatched completed frame IDs, clip IDs or timing validity")
    if [row["frame"] for row in a] != expected_frame_ids(reference):
        raise ValueError("Incomplete playback telemetry: IDs differ from source capture range")
    paired = [(left, right) for left, right in zip(a[warmup:], b[warmup:]) if left["valid"]]
    if not paired:
        raise ValueError("No matched valid intervals after equal warmup")
    timing = {}
    for key in ("wall_ms", "cpu_ms", "process_cpu_ms"):
        left = distribution([row[0][key] for row in paired])
        right = distribution([row[1][key] for row in paired])
        timing[key] = {"reference": left, "candidate": right,
                       "mean_change_percent": (right["mean"] / left["mean"] - 1) * 100 if left["mean"] else None,
                       "speedup": left["mean"] / right["mean"] if right["mean"] else None,
                       "paired_delta_ms": distribution([row[1][key] - row[0][key] for row in paired])}
    start_a, start_b = (read_verification(path / "start-verification.json") for path in (reference, candidate))
    if not start_a["matches"] or not start_b["matches"]:
        raise ValueError("Playback did not restore its recorded starting state")
    end_a, end_b = (read_verification(path / "verification.json") for path in (reference, candidate))
    start = semantic(start_a, start_b, position_tolerance, camera_tolerance)
    end = semantic(end_a, end_b, position_tolerance, camera_tolerance)
    image_a, image_b = (image(path / "actual-end.ppm") for path in (reference, candidate))
    image_matches = image_a == image_b
    return {"schema": 1, "reference": str(reference), "candidate": str(candidate), "capture_sha256": source,
            "warmup_completed_frames": warmup, "total_completed_frames": len(a), "matched_valid_intervals": len(paired),
            "first_measured_frame": paired[0][0]["frame"], "last_measured_frame": paired[-1][0]["frame"],
            "timing": timing, "game_fps": {"reference": 1000 / timing["wall_ms"]["reference"]["mean"],
                                           "candidate": 1000 / timing["wall_ms"]["candidate"]["mean"]},
            "start": start, "endpoint": end,
            "endpoint_image": {"reference": image_a, "candidate": image_b, "matches": image_matches},
            "original_capture_comparison": {"reference": end_a, "candidate": end_b,
                                            "note": "Original recorded expectations are immutable; their DRIFT is separate from corrected-reference parity."},
            "parity_passed": start["matches"] and end["matches"] and image_matches,
            "notes": ["FPS counts completed game intervals, not window redraws.",
                      "Process CPU includes other process threads; game-thread CPU is reported separately.",
                      "GPU elapsed query observations are delayed and are not paired as exact frame latency.",
                      "Run comparisons sequentially without competing games, builds or profilers; repeat pairs before accepting a gain."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--warmup", type=int, default=60, help="Equal leading completed-frame rows to omit (default 60)")
    parser.add_argument("--output", "-o", type=Path)
    args = parser.parse_args(argv)
    try:
        result = compare(args.reference, args.candidate, args.warmup)
        output = json.dumps(result, indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(output)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(2, f"compare_replays: {error}\n")
    print(output, end="")
    return 0 if result["parity_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
