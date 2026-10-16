"""Fail closed when native optimization evidence is incomplete or incompatible."""
import copy
import csv
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("compare_replays", ROOT / "tools/compare_replays.py")
COMPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARE)


def endpoint(x=10, original_x=10):
    state = {"mode": 11, "state": 1, "ostrich": 2, "scene": 123, "player_valid": 1,
             "camera_valid": 0, "health": 2, "snacks": 203, "keys": 27, "control": 1,
             "position_0": x, "position_1": 2, "position_2": 3, "position_distance": abs(x - original_x)}
    fields = {}
    for key, value in state.items():
        expected = original_x if key == "position_0" else 0 if key == "position_distance" else value
        tolerance = .1 if key.startswith("position_") else 0
        error = abs(value - expected)
        fields[key] = {"expected": expected, "actual": value, "error": error,
                       "tolerance": tolerance, "matches": error <= tolerance}
    return {"version": 1, "position_tolerance": .1, "camera_tolerance": .02,
            "fields": fields, "matches": all(field["matches"] for field in fields.values())}


class CompareReplays(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.session = self.base / "session-000004"
        self.left, self.right = (self.session / "replays" / name for name in ("reference", "candidate"))
        for path in (self.left, self.right):
            path.mkdir(parents=True)
            (path / "start-verification.json").write_text(json.dumps(endpoint()))
            (path / "verification.json").write_text(json.dumps(endpoint(x=9.73)))
            (path / "actual-end.ppm").write_bytes(b"P6\n2 1\n255\n" + bytes((10, 20, 30)) * 2)
        for name in COMPARE.CAPTURE_FILES:
            (self.session / name).write_text(name)
        (self.session / "metadata.txt").write_text('NIGHT100_SESSION 1 4 "elf" "options" 0 12 1\n')
        self.rows = [{"frame": i, "wall_ms": 0 if i == 1 else 16.67,
                      "cpu_ms": 40 if i <= 2 else 5, "process_cpu_ms": 8,
                      "timing_valid": 0 if i == 1 else 1} for i in range(1, 13)]
        self.write_rows(self.left, self.rows)
        self.write_rows(self.right, [{**row, "cpu_ms": row["cpu_ms"] / 5, "process_cpu_ms": 4} for row in self.rows])

    def write_rows(self, directory, rows):
        with (directory / "frames.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0].keys() if rows else self.rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)

    def compare(self, **kwargs):
        return COMPARE.compare(self.left, self.right, warmup=kwargs.pop("warmup", 2), **kwargs)

    def modify_json(self, name, function):
        path = self.right / name
        data = json.loads(path.read_text())
        function(data)
        path.write_text(json.dumps(data))

    def test_equal_frame_warmup_cpu_process_and_tail_metrics(self):
        result = self.compare()
        self.assertTrue(result["parity_passed"])
        self.assertEqual(result["matched_valid_intervals"], 10)
        self.assertEqual(result["first_measured_frame"], 3)
        self.assertEqual(result["last_measured_frame"], 12)
        cpu = result["timing"]["cpu_ms"]
        self.assertEqual(cpu["speedup"], 5)
        self.assertEqual(cpu["mean_change_percent"], -80)
        self.assertEqual(cpu["reference"]["p95"], 5)
        self.assertEqual(cpu["candidate"]["p99"], 1)
        self.assertEqual(cpu["paired_delta_ms"]["mean"], -4)
        self.assertEqual(result["timing"]["process_cpu_ms"]["speedup"], 2)
        self.assertAlmostEqual(result["game_fps"]["reference"], 1000 / 16.67)

    def test_original_capture_drift_is_separate_and_files_immutable(self):
        before = (self.right / "verification.json").read_bytes()
        result = self.compare()
        self.assertFalse(result["original_capture_comparison"]["candidate"]["matches"])
        self.assertTrue(result["endpoint"]["matches"])
        self.assertEqual((self.right / "verification.json").read_bytes(), before)

    def test_sparse_endpoint_is_rejected(self):
        self.modify_json("verification.json", lambda data: data["fields"].pop("health"))
        with self.assertRaisesRegex(ValueError, "missing or unknown"):
            self.compare()

    def test_camera_valid_requires_all_twelve_components(self):
        self.modify_json("verification.json", lambda data: data["fields"]["camera_valid"].update(actual=1, expected=1))
        with self.assertRaisesRegex(ValueError, "missing or unknown"):
            self.compare()

    def test_endpoint_drift_and_image_difference_fail_parity(self):
        (self.right / "verification.json").write_text(json.dumps(endpoint(x=8)))
        result = self.compare()
        self.assertFalse(result["parity_passed"])
        self.assertFalse(result["endpoint"]["matches"])
        self.assertTrue(result["endpoint_image"]["matches"])
        (self.right / "verification.json").write_text(json.dumps(endpoint(x=9.73)))
        (self.right / "actual-end.ppm").write_bytes(b"P6\n2 1\n255\n" + bytes(6))
        result = self.compare()
        self.assertTrue(result["endpoint"]["matches"])
        self.assertFalse(result["endpoint_image"]["matches"])
        self.assertFalse(result["parity_passed"])

    def test_position_uses_euclidean_distance(self):
        report = endpoint(x=9.79)
        report["fields"]["position_1"]["actual"] += .09
        (self.right / "verification.json").write_text(json.dumps(report))
        result = self.compare()
        self.assertTrue(result["endpoint"]["fields"]["position_0"]["matches"])
        self.assertTrue(result["endpoint"]["fields"]["position_1"]["matches"])
        self.assertFalse(result["endpoint"]["fields"]["position_distance"]["matches"])

    def test_missing_duplicate_and_out_of_order_frames_rejected(self):
        for rows in (self.rows[:4] + self.rows[5:], self.rows[:4] + [self.rows[3]] + self.rows[4:], list(reversed(self.rows))):
            with self.subTest(rows=rows[:2]):
                self.write_rows(self.right, rows)
                with self.assertRaisesRegex(ValueError, "duplicate, missing or out-of-order"):
                    self.compare()

    def test_truncated_or_shifted_complete_frame_sequences_rejected(self):
        self.write_rows(self.right, self.rows[:-1])
        with self.assertRaisesRegex(ValueError, "Mismatched completed"):
            self.compare()
        self.write_rows(self.left, self.rows[:-1])
        with self.assertRaisesRegex(ValueError, "Incomplete playback"):
            self.compare()
        self.write_rows(self.left, self.rows)
        self.write_rows(self.right, [{**row, "frame": row["frame"] + 1} for row in self.rows])
        with self.assertRaisesRegex(ValueError, "Mismatched completed"):
            self.compare()

    def test_mismatched_validity_or_clip_ids_rejected(self):
        rows = copy.deepcopy(self.rows)
        rows[4]["timing_valid"] = 0
        self.write_rows(self.right, rows)
        with self.assertRaisesRegex(ValueError, "timing validity"):
            self.compare()
        self.write_rows(self.right, [{**row, "clip_frame": row["frame"]} for row in self.rows])
        with self.assertRaisesRegex(ValueError, "clip IDs"):
            self.compare()

    def test_incompatible_source_capture_rejected(self):
        other = self.base / "other-session"
        shutil.copytree(self.session, other)
        self.right = other / "replays" / "candidate"
        (other / "clock.bin").write_bytes(b"different clock")
        with self.assertRaisesRegex(ValueError, "Incompatible source captures"):
            self.compare()

    def test_missing_and_truncated_images_rejected(self):
        path = self.right / "actual-end.ppm"
        path.unlink()
        with self.assertRaises(OSError):
            self.compare()
        path.write_bytes(b"P6\n2 1\n255\n\x01")
        with self.assertRaisesRegex(ValueError, "incomplete endpoint"):
            self.compare()

    def test_bad_json_duplicate_keys_and_nonfinite_rejected(self):
        for text in ('{', '[]', '{"version":1,"version":1}', '{"version":NaN}', '{"version":Infinity}'):
            with self.subTest(text=text):
                (self.right / "verification.json").write_text(text)
                with self.assertRaises(ValueError):
                    self.compare()

    def test_invalid_json_fields_rejected(self):
        original = (self.right / "verification.json").read_text()
        changes = [lambda data: data.update(version=True), lambda data: data.update(matches=True),
                   lambda data: data["fields"]["health"].update(actual=True),
                   lambda data: data["fields"]["scene"].update(actual=1.5),
                   lambda data: data["fields"]["keys"].update(tolerance=-1),
                   lambda data: data["fields"]["keys"].update(actual=float("inf"))]
        for change in changes:
            with self.subTest(change=change):
                (self.right / "verification.json").write_text(original)
                self.modify_json("verification.json", change)
                with self.assertRaises(ValueError):
                    self.compare()

    def test_invalid_timing_columns_rows_numbers_and_warmup(self):
        for text in ("frame,cpu_ms\n1,2\n", "frame,wall_ms,cpu_ms,process_cpu_ms,cpu_ms\n1,2,3,4,5\n",
                     "frame,wall_ms,cpu_ms,process_cpu_ms\n1,nan,2,3\n", "frame,wall_ms,cpu_ms,process_cpu_ms\n1,2,-1,3\n",
                     "frame,wall_ms,cpu_ms,process_cpu_ms\n1,2\n"):
            with self.subTest(text=text):
                (self.right / "frames.csv").write_text(text)
                with self.assertRaises(ValueError):
                    self.compare()
        self.write_rows(self.right, self.rows)
        for warmup in (-1, True, 12, 100):
            with self.subTest(warmup=warmup), self.assertRaises(ValueError):
                self.compare(warmup=warmup)

    def test_failed_start_restore_rejected(self):
        (self.right / "start-verification.json").write_text(json.dumps(endpoint(x=9)))
        with self.assertRaisesRegex(ValueError, "starting state"):
            self.compare()

    def test_cli_reports_exit_status_and_output(self):
        command = [sys.executable, str(ROOT / "tools/compare_replays.py"), str(self.left), str(self.right), "--warmup", "2"]
        output = self.base / "comparison.json"
        result = subprocess.run([*command, "--output", str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), json.loads(output.read_text()))
        (self.right / "verification.json").write_text(json.dumps(endpoint(x=1)))
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(json.loads(result.stdout)["parity_passed"])
        (self.right / "verification.json").write_text("invalid")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("compare_replays:", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
