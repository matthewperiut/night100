"""Numbered replay reports must expose stalls and delayed GPU observations."""
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("replay_report", ROOT / "tools/replay_report.py")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


def row(frame, wall=1000/60, cpu=2, process=3, gpu="", valid="1", clip=None):
    result = dict(frame=str(frame), wall_ms=str(wall), cpu_ms=str(cpu), process_cpu_ms=str(process), gpu_ms=str(gpu))
    if valid is not None:
        result["timing_valid"] = valid
    if clip is not None:
        result["clip_frame"] = str(clip)
    return result


class ReplayReport(unittest.TestCase):
    def test_stalls_have_separate_wall_and_cpu_rankings(self):
        rows = [row(i, clip=i-100) for i in range(101, 121)]
        rows[4] = row(105, 500, 3, 6, clip=5)  # blocked, not CPU work
        rows[12] = row(113, 40, 30, 35, clip=13)
        report = REPORT.summarize(rows)
        self.assertEqual(report["valid_completed_intervals"], 20)
        self.assertLess(report["game_fps"], 30)
        self.assertEqual(report["wall_ms"]["max"], 500)
        self.assertEqual(report["cpu_ms"]["p99"], 30)
        self.assertEqual(report["process_cpu_ms"]["p99"], 35)
        self.assertEqual(report["worst_wall_intervals"][0]["frame"], 105)
        self.assertEqual(report["worst_cpu_intervals"][0]["frame"], 113)
        self.assertEqual(report["worst_cpu_intervals"][0]["clip_frame"], 13)
        self.assertEqual(len(report["worst_wall_intervals"]), 10)
        self.assertAlmostEqual(report["worst_wall_intervals"][0]["interval_end_seconds"], 4/60+.5)

    def test_blank_gpu_is_unavailable_not_zero(self):
        report = REPORT.summarize([row(i) for i in range(1, 61)])
        self.assertAlmostEqual(report["game_fps"], 60)
        gpu = report["gpu_delayed_query_work"]
        self.assertEqual(gpu["observed_intervals"], 0)
        self.assertIsNone(gpu["mean_ms_per_valid_completed_interval"])
        self.assertIsNone(gpu["max_observed_interval_ms"])

    def test_switch_missing_process_cpu_keeps_producer_benchmark(self):
        report = REPORT.summarize([row(i, 20, 10, process="") for i in range(1, 61)])
        self.assertEqual(report["valid_completed_intervals"], 60)
        self.assertEqual(report["game_fps"], 50)
        self.assertEqual(report["cpu_ms"]["mean"], 10)
        self.assertEqual(report["process_cpu_available_intervals"], 0)
        self.assertIsNone(report["process_cpu_ms"]["mean"])

    def test_gpu_results_are_delayed_observations(self):
        rows = [row(1, 200, 3), row(2, 10, 2, gpu=5), row(3, 10, 1, gpu=0)]
        report = REPORT.summarize(rows)
        gpu = report["gpu_delayed_query_work"]
        self.assertEqual(gpu["observed_intervals"], 2)
        self.assertEqual(gpu["mean_ms_per_observed_interval"], 2.5)
        self.assertAlmostEqual(gpu["mean_ms_per_valid_completed_interval"], 5/3)
        self.assertEqual(gpu["largest_observation_intervals"][0]["frame"], 2)
        self.assertEqual(report["worst_wall_intervals"][0]["frame"], 1)
        self.assertIn("not exact per-frame", gpu["interpretation"])
        self.assertNotIn("gpu_caused", json.dumps(report))

    def test_invalid_capture_intervals_do_not_hide_or_create_fps(self):
        rows = [row(1, 0, 0, 0, valid="0"), row(2, 1000, 900, 950, gpu=50, valid="false"),
                row(3, 20, 2, 3), row(4, float("nan")), row(5, -1), row(6, 20, -1),
                row(7, 20, process="inf"), row(8, 20, gpu="nan"), row(9, 20, gpu=-2)]
        report = REPORT.summarize(rows)
        self.assertEqual(report["valid_completed_intervals"], 3)
        self.assertEqual(report["excluded_intervals"], 6)
        self.assertEqual(report["game_fps"], 50)
        self.assertEqual(report["wall_ms"]["max"], 20)
        self.assertEqual(report["gpu_delayed_query_work"]["invalid_observations"], 2)
        self.assertFalse(report["timestamps"]["complete"])
        self.assertGreater(report["timestamps"]["covered_seconds"], 1)
        json.dumps(report, allow_nan=False)

    def test_one_replay_interval_without_validity_column(self):
        report = REPORT.summarize([row(0, 0, 0, 0, valid=None), row(1, 25, 4, 7, valid=None)])
        self.assertEqual(report["valid_completed_intervals"], 1)
        self.assertEqual(report["game_fps"], 40)
        self.assertEqual(report["wall_ms"]["p50"], 25)
        self.assertEqual(report["wall_ms"]["p99"], 25)
        self.assertEqual(report["cpu_ms"]["p95"], 4)

    def test_jitter_uses_sum_of_intervals_not_mean_instant_fps(self):
        report = REPORT.summarize([row(i+1, wall) for i, wall in enumerate([10, 20]*30)])
        self.assertAlmostEqual(report["game_fps"], 1000/15)
        self.assertEqual(report["wall_ms"]["p50"], 10)
        self.assertEqual(report["wall_ms"]["p95"], 20)
        self.assertNotEqual(report["game_fps"], (100+50)/2)

    def test_empty_or_duplicate_intervals_are_explicit(self):
        empty = REPORT.summarize([])
        self.assertIsNone(empty["game_fps"])
        self.assertEqual(empty["worst_cpu_intervals"], [])
        duplicate = REPORT.summarize([row(1), row(1), row(2)])
        self.assertEqual(duplicate["valid_completed_intervals"], 2)
        self.assertEqual(duplicate["exclusion_reasons"]["non_increasing_frame_id"], 1)

    def test_cli_accepts_directory_and_replay_csv(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fields = ["frame", "clip_frame", "wall_ms", "cpu_ms", "process_cpu_ms", "gpu_ms", "gpu_queries", "readbacks", "texture_uploads", "timing_valid"]
            with (directory/"frames.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                writer.writerow(row(500, 0, 0, 0, valid="0", clip=0))
                writer.writerow(row(501, 20, 3, 4, gpu=.5, clip=1))
            output = directory/"report.json"
            result = subprocess.run([sys.executable, str(ROOT/"tools/replay_report.py"), str(directory), "--output", str(output)], capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout), json.loads(output.read_text()))
            self.assertEqual(json.loads(result.stdout)["game_fps"], 50)
            result = subprocess.run([sys.executable, str(ROOT/"tools/replay_report.py"), str(directory/"frames.csv")], capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)["valid_completed_intervals"], 1)
            (directory/"frames.csv").write_text("frame,wall_ms\n1,20\n")
            bad = subprocess.run([sys.executable, str(ROOT/"tools/replay_report.py"), str(directory)], capture_output=True, text=True)
            self.assertEqual(bad.returncode, 2)
            self.assertIn("Missing telemetry columns", bad.stderr)

    def test_schema_two_phase_split_and_fast_slow_halves(self):
        phases = list(REPORT.PHASES)
        rows = []
        for i in range(1, 21):
            slow = i > 10
            row_values = row(i, 20, 16 if slow else 10)
            row_values["frame_schema"] = "2"
            for name in phases:
                row_values[f"{name}_ms"] = "0"
            row_values["update_ms"] = str(8 if slow else 5)
            row_values["render_ms"] = str(6 if slow else 4)
            row_values["present_ms"] = "4"
            row_values["update_cpu_ms"] = str(7.5 if slow else 4.5)
            row_values["render_cpu_ms"] = str(5.5 if slow else 3.5)
            row_values["present_cpu_ms"] = "0.5"
            rows.append(row_values)
        rows[0]["render_ms"] = ""  # a baseline row without a split stays out of the phase means
        columns = REPORT.phase_columns(list(rows[0]))
        report = REPORT.summarize(rows, columns)
        split = report["frame_phases"]
        self.assertEqual(split["intervals"], 20)
        self.assertAlmostEqual(split["all"]["update_ms"]["mean"], 6.5)
        self.assertAlmostEqual(split["all"]["render_ms"]["mean"], (4*9+6*10)/19)
        self.assertEqual(split["fast_half_by_cpu"]["update_ms"]["mean"], 5)
        self.assertEqual(split["slow_half_by_cpu"]["update_ms"]["mean"], 8)
        self.assertEqual(split["slow_half_by_cpu"]["update_ms"]["p95"], 8)
        self.assertEqual(split["slow_half_by_cpu"]["render_cpu_ms"]["mean"], 5.5)
        self.assertNotIn("phases", report["worst_cpu_intervals"][0])
        self.assertTrue(any("update_ms 6.50" in line for line in REPORT.phase_lines({"frame_phases": split})))
        json.dumps(report, allow_nan=False)

    def test_schema_one_files_have_no_phase_section(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/"frames.csv"
            path.write_text("frame,wall_ms,cpu_ms,process_cpu_ms,gpu_ms\n1,0,0,,\n2,16,10,,\n")
            report = REPORT.report(path)
            self.assertEqual(report["frame_schema"], 1)
            self.assertNotIn("frame_phases", report)
            self.assertEqual(report["valid_completed_intervals"], 1)


if __name__ == "__main__":
    unittest.main()
