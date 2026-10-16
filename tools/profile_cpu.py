#!/usr/bin/env python3
"""Collect a fresh bounded macOS game profile, or analyze an existing sample.

Examples (run collection without another game, build, or profiler):
  python3 tools/profile_cpu.py analyze logs/cpu-wave/profile.sample.txt
  python3 tools/profile_cpu.py collect --output logs/profile-new --sample-seconds 8 -- \
    build/mac/night100 game/SLUS_203.49 --replay-session logs/replays/session-000004 --seconds 15
  python3 tools/profile_cpu.py collect --output logs/timing-new -- \
    build/mac/night100 game/SLUS_203.49 --replay-session logs/replays/session-000004 --seconds 15

Collection never builds, overwrites a run, attaches to an existing process, or
terminates a process. --seconds is mandatory. Run sampled and unsampled timing
experiments separately; wall-stack weights cannot be converted to CPU ms.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

NOTE = ("Wall-stack observations, including sleeps and driver waits; neither "
        "inclusive nor exclusive weights are CPU-time percentages. Do not infer "
        "CPU milliseconds or speedup from this sampled run. Measure CPU separately "
        "with unprofiled completed-frame telemetry.")
THREAD = re.compile(r"^\s*(\d+) Thread_[^\s:]+(?:: (.*)|\s+.*)?$")
FRAME = re.compile(r"^([ +!:|]*)(\d+) (.+)$")


def analyze(text, thread_name="GameThread"):
    """Aggregate every exact named thread and every scheduler branch.

    Symbol-inclusive counts deduplicate recursive occurrences within each leaf
    stack. Occurrence-inclusive counts are separate and may exceed thread counts.
    """
    roots, stack, current, reading = [], [], None, False
    for line in text.splitlines():
        if line.strip() == "Call graph:":
            reading = True
            continue
        if not reading:
            continue
        match = THREAD.match(line)
        if match:
            count, name = match.groups()
            name = (name or "").split("   DispatchQueue_", 1)[0].strip()
            current = {"name": name, "count": int(count), "children": [], "depth": -1}
            if name == thread_name:
                roots.append(current)
            stack = [current]
            continue
        if line and not line[0].isspace():
            break
        match = FRAME.match(line)
        if not match or current is None or current["name"] != thread_name:
            continue
        prefix, count, raw = match.groups()
        depth = len(prefix)
        while len(stack) > 1 and stack[-1]["depth"] >= depth:
            stack.pop()
        node = {"name": raw.split("  (in ", 1)[0], "count": int(count),
                "children": [], "depth": depth}
        stack[-1]["children"].append(node)
        stack.append(node)
    if not roots:
        raise ValueError(f"No exact thread named {thread_name!r} in sample call graph")
    exclusive, inclusive, occurrences = Counter(), Counter(), Counter()
    leaves, warnings = [], []
    # Iterative traversal handles deeply recursive translated dispatch stacks.
    pending = [(root, []) for root in roots]
    while pending:
        node, path = pending.pop()
        is_root = node["depth"] == -1
        path = path if is_root else path + [node["name"]]
        children_count = sum(child["count"] for child in node["children"])
        own = node["count"] - children_count
        if own < 0:
            warnings.append({"symbol": node["name"], "count": node["count"],
                             "child_count": children_count})
        if not is_root:
            occurrences[node["name"]] += node["count"]
        if own > 0:
            name = path[-1] if path else "<unresolved thread samples>"
            exclusive[name] += own
            inclusive.update({name: own for name in set(path)})
            leaves.append({"count": own, "name": name, "stack": path})
        pending.extend((child, path) for child in node["children"])
    return {"thread_name": thread_name, "matching_threads": len(roots),
            "gameThreadSamples": sum(root["count"] for root in roots),
            "sample_semantics": NOTE,
            "exclusive": dict(exclusive.most_common()),
            "inclusive": dict(inclusive.most_common()),
            "inclusive_occurrences": dict(occurrences.most_common()),
            "exclusive_total": sum(exclusive.values()),
            "branch_count_warnings": warnings,
            "leaf_stacks": sorted(leaves, key=lambda item: -item["count"])}


def bounded_seconds(command):
    """Only launch the project's self-bounded executable, never a shell wrapper."""
    if len(command) < 3:
        raise ValueError("Supply a night100 executable and --seconds N")
    if Path(command[0]).name != "night100":
        raise ValueError("Collection requires the night100 executable directly")
    if any(flag in command for flag in ("--record-inputs", "--record-gs", "--dump-state", "--capture")):
        raise ValueError("Collection cannot record or write existing capture paths; use replay inputs")
    indices = [i for i, argument in enumerate(command) if argument == "--seconds"]
    if len(indices) != 1 or indices[0] + 1 == len(command):
        raise ValueError("Exactly one --seconds N is required")
    try:
        seconds = float(command[indices[0] + 1])
    except ValueError as error:
        raise ValueError("--seconds must be a finite number in (0, 120]") from error
    if not math.isfinite(seconds) or not 0 < seconds <= 120:
        raise ValueError("--seconds must be a finite number in (0, 120]")
    return seconds


def clean_environment(environ):
    # Reference switches remain intentional experiment controls and are recorded.
    prefixes = ("NIGHT100_BENCHMARK", "NIGHT100_AUTOPLAY", "NIGHT100_TRACE",
                "NIGHT100_COLLISION_DEBUG", "NIGHT100_REPLAY_FRAME_DUMPS",
                "NIGHT100_PROFILE", "NIGHT100_CAPTURE_", "NIGHT100_SESSION_CAPTURE_FRAMES")
    return {key: value for key, value in environ.items() if not key.startswith(prefixes)}


def competing_processes(text, own_pid):
    competitors = []
    names = {"night100", "baseline-night100", "cmake", "ninja", "clang", "clang++", "sample"}
    for line in text.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) != 2 or not parts[0].isdigit():
            continue
        if int(parts[0]) != own_pid and Path(parts[1]).name in names:
            competitors.append({"pid": int(parts[0]), "command": parts[1]})
    return competitors


def write_new(path, data):
    with Path(path).open("x") as stream:
        stream.write(json.dumps(data, indent=2) + "\n")


def collect(output, command, sample_seconds=None, delay=1.2):
    duration = bounded_seconds(command)
    if sample_seconds is not None and (sample_seconds < 1 or delay + sample_seconds >= duration):
        raise ValueError("Sample duration plus delay must be shorter than --seconds")
    if not math.isfinite(delay) or delay < 0:
        raise ValueError("Sample delay must be finite and nonnegative")
    if sys.platform != "darwin":
        raise ValueError("The collector requires macOS /usr/bin/sample")
    binary = Path(command[0]).resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("Executable is not an executable regular file")
    # The archived executable moves, so preserve its ELF lookup explicitly.
    if command[1].startswith("-") or not Path(command[1]).is_file():
        raise ValueError("Provide an explicit ELF path immediately after the executable")
    elf = str(Path(command[1]).resolve())
    if sample_seconds is not None and not Path("/usr/bin/sample").is_file():
        raise ValueError("macOS /usr/bin/sample is unavailable")
    # Check before touching output or starting anything. No existing PID is used.
    processes = subprocess.check_output(["/bin/ps", "-axo", "pid=,comm="], text=True)
    competitors = competing_processes(processes, os.getpid())
    if competitors:
        raise ValueError(f"Close competing game/build/profiler processes first: {competitors}")
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    frozen_binary = output / "night100"
    shutil.copy2(binary, frozen_binary)
    digest = hashlib.sha256(frozen_binary.read_bytes()).hexdigest()
    command = [str(frozen_binary), elf, *command[2:]]
    environment = clean_environment(os.environ)
    manifest = {"binary": str(frozen_binary), "sha256": digest, "command": command,
                "cwd": os.getcwd(), "sampled": sample_seconds is not None,
                "sample_seconds": sample_seconds, "sample_delay_seconds": delay,
                "sample_semantics": NOTE, "controls": {
                    key: value for key, value in environment.items() if key.startswith("NIGHT100_")}}
    write_new(output / "manifest.json", manifest)
    try:
        with (output / "game.log").open("x") as game_log:
            child = subprocess.Popen(command, env=environment, stdout=game_log, stderr=subprocess.STDOUT)
            manifest["pid"] = child.pid
            if sample_seconds is not None:
                time.sleep(delay)
                if child.poll() is not None:
                    raise RuntimeError("Game exited before profiling; inspect game.log")
                with (output / "sampler.log").open("x") as sampler_log:
                    sampler = subprocess.Popen(["/usr/bin/sample", str(child.pid), str(sample_seconds),
                                                "1", "-file", str(output / "sample.txt")],
                                               stdout=sampler_log, stderr=subprocess.STDOUT)
                    try:
                        manifest["sampler_exit"] = sampler.wait(timeout=sample_seconds + 20)
                    except subprocess.TimeoutExpired:
                        manifest["sampler_timeout_pid"] = sampler.pid
                        manifest["error"] = "Sampler did not exit; process was not terminated"
            try:
                manifest["exit"] = child.wait(timeout=duration + 20)
            except subprocess.TimeoutExpired:
                manifest["timeout_pid"] = child.pid
                manifest["error"] = "Self-bounded game did not exit; process was not terminated"
        if sample_seconds is not None:
            if manifest.get("sampler_exit") != 0 or not (output / "sample.txt").is_file():
                manifest["error"] = "Sampler failed; inspect sampler.log"
            elif "error" not in manifest:
                write_new(output / "sample.json", analyze((output / "sample.txt").read_text()))
    except (OSError, ValueError, RuntimeError) as error:
        manifest["error"] = str(error)
        raise
    finally:
        # Keep immutable initial intent and final/failure records, including PID.
        write_new(output / "result.json", manifest)
    if "error" in manifest or manifest.get("exit") not in (0, 2):
        raise RuntimeError(f"Run did not finish normally; inspect {output / 'result.json'}")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    modes = parser.add_subparsers(dest="mode", required=True)
    analysis = modes.add_parser("analyze")
    analysis.add_argument("sample", type=Path)
    analysis.add_argument("--thread", default="GameThread")
    analysis.add_argument("--output", type=Path)
    capture = modes.add_parser("collect")
    capture.add_argument("--output", type=Path, required=True)
    capture.add_argument("--sample-seconds", type=int)
    capture.add_argument("--delay", type=float, default=1.2)
    capture.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        if args.mode == "analyze":
            result = analyze(args.sample.read_text(), args.thread)
            if args.output:
                write_new(args.output, result)
            else:
                print(json.dumps(result, indent=2))
        else:
            command = args.command[1:] if args.command[:1] == ["--"] else args.command
            print(json.dumps(collect(args.output, command, args.sample_seconds, args.delay), indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
