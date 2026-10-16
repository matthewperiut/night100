"""Profile parsing covers whole exact GameThread trees, never one scheduler branch."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("profile_cpu", ROOT / "tools/profile_cpu.py")
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)

SAMPLE = """Analysis of sampling test
Call graph:
    99 Thread_1: Main Thread
    + 99 unrelated  (in binary) + 0 [0x10]
    10 Thread_2: GameThread
    + 10 thread_start  (in binary) + 0 [0x10]
    +   6 EeScheduler::run()  (in binary) + 40 [0x40]
    +   ! 6 sleep  (in system) + 0 [0x50]
    +   4 EeScheduler::run()  (in binary) + 80 [0x80]
    +     3 dispatch  (in binary) + 0 [0x90]
    +     ! 2 dispatch  (in binary) + 0 [0x91]
    +     !   2 collision  (in binary) + 0 [0x92]
    +     1 timer  (in binary) + 0 [0x93]
    22 Thread_33
    + 22 unnamed_thread  (in binary)
    23 Thread_34   DispatchQueue_1: queue
    + 23 dispatch_thread  (in binary)
    7 Thread_3: GameThread Worker
    + 7 impostor  (in binary) + 0 [0x10]
Sort by top of stack, same collapsed:
    1000 fake
"""


class ProfileCpu(unittest.TestCase):
    def test_all_scheduler_branches_and_exact_thread(self):
        result = PROFILE.analyze(SAMPLE)
        self.assertEqual(result["gameThreadSamples"], 10)
        self.assertEqual(result["exclusive"], {"sleep": 6, "collision": 2, "dispatch": 1, "timer": 1})
        self.assertEqual(result["inclusive"]["EeScheduler::run()"], 10)
        self.assertEqual(result["exclusive_total"], 10)
        self.assertEqual(result["branch_count_warnings"], [])
        self.assertNotIn("impostor", result["exclusive"])
        self.assertNotIn("unrelated", result["inclusive"])

    def test_recursive_inclusive_not_counted_twice(self):
        result = PROFILE.analyze(SAMPLE)
        self.assertEqual(result["inclusive"]["dispatch"], 3)
        self.assertEqual(result["inclusive_occurrences"]["dispatch"], 5)

    def test_multiple_exact_threads_are_aggregated(self):
        text = SAMPLE.split("Sort by top", 1)[0] + "    2 Thread_4: GameThread\n    + 2 timer  (in game)\n"
        result = PROFILE.analyze(text)
        self.assertEqual(result["matching_threads"], 2)
        self.assertEqual(result["gameThreadSamples"], 12)
        self.assertEqual(result["exclusive"]["timer"], 3)

    def test_missing_thread_is_error(self):
        with self.assertRaisesRegex(ValueError, "No exact thread"):
            PROFILE.analyze(SAMPLE.replace(": GameThread", ": Renamed"))

    def test_missing_graph_is_error(self):
        with self.assertRaises(ValueError):
            PROFILE.analyze("invalid sample")

    def test_count_mismatch_is_visible(self):
        result = PROFILE.analyze(SAMPLE.replace("+   6 EeScheduler", "+   5 EeScheduler"))
        self.assertTrue(result["branch_count_warnings"])
        self.assertIn("not", result["sample_semantics"])
        self.assertNotIn("cpu_ms", result)

    def test_deep_tree_does_not_recurse_python_stack(self):
        tree = "Call graph:\n    1 Thread_4: GameThread\n"
        tree += "\n".join("    + " + "  " * i + "1 dispatch  (in game)" for i in range(1100))
        result = PROFILE.analyze(tree)
        self.assertEqual(result["inclusive"]["dispatch"], 1)
        self.assertEqual(result["inclusive_occurrences"]["dispatch"], 1100)

    def test_existing_output_never_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "evidence.json"
            path.write_text("original")
            with self.assertRaises(FileExistsError):
                PROFILE.write_new(path, {"overwrite": True})
            self.assertEqual(path.read_text(), "original")

    def test_diagnostic_environment_removed_reference_controls_preserved(self):
        env = {"PATH": "bin", "NIGHT100_TRACE_RW": "1", "NIGHT100_PROFILE_FRAMES": "1",
               "NIGHT100_REFERENCE_EE_TIMERS": "1", "NIGHT100_CAPTURE_COMPLETED": "x",
               "NIGHT100_AUTOPLAY": "1", "NIGHT100_SESSION_CAPTURE_FRAMES": "1:2"}
        self.assertEqual(PROFILE.clean_environment(env), {
            "PATH": "bin", "NIGHT100_REFERENCE_EE_TIMERS": "1"})

    def test_competitor_names_and_pid_no_parent_arguments_false_match(self):
        text = "1 /sbin/launchd\n12 /repo/build/mac/night100\n13 /bin/ninja\n14 /usr/bin/python3\n"
        self.assertEqual(PROFILE.competing_processes(text, 13), [{"pid": 12, "command": "/repo/build/mac/night100"}])

    def test_bounded_command_required(self):
        for args in (["night100"], ["sh", "-c", "night100"],
                     ["night100", "--seconds", "NaN"], ["night100", "--seconds", "0"],
                     ["night100", "--seconds", "121"],
                     ["night100", "--seconds", "2", "--record-inputs", "existing.inputs"],
                     ["night100", "--seconds", "2", "--seconds", "3"]):
            with self.assertRaises(ValueError):
                PROFILE.bounded_seconds(args)
        self.assertEqual(PROFILE.bounded_seconds(["build/mac/night100", "game/ELF", "--seconds", "15"]), 15)

    def test_fresh_collector_archives_binary_and_own_pid_without_cpu_claim(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            binary, elf, output = base / "night100", base / "ELF", base / "fresh"
            binary.write_text("fake executable"); binary.chmod(0o755); elf.write_text("fake ELF")
            class Child:
                pid = 12345
                def wait(self, timeout):
                    return 2  # existing physics-corrected replay endpoint drift
            with patch.object(PROFILE.sys, "platform", "darwin"), \
                    patch.object(PROFILE.subprocess, "check_output", return_value=""), \
                    patch.object(PROFILE.subprocess, "Popen", return_value=Child()) as launch:
                result = PROFILE.collect(output, [str(binary), str(elf), "--seconds", "3"])
                self.assertEqual(result["pid"], 12345)
                self.assertFalse(result["sampled"])
                self.assertNotIn("cpu_ms", result)
                self.assertEqual(launch.call_args.args[0][0], str(output.resolve() / "night100"))
                self.assertEqual((output / "night100").read_bytes(), binary.read_bytes())
                self.assertEqual(json.loads((output / "result.json").read_text())["exit"], 2)
                with self.assertRaises(FileExistsError):
                    PROFILE.collect(output, [str(binary), str(elf), "--seconds", "3"])
                self.assertEqual(launch.call_count, 1)

    def test_collector_invalid_elf_fails_before_creating_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder); binary, output = base / "night100", base / "fresh"
            binary.write_text("fake executable"); binary.chmod(0o755)
            with patch.object(PROFILE.sys, "platform", "darwin"), \
                    patch.object(PROFILE.subprocess, "Popen") as launch:
                with self.assertRaisesRegex(ValueError, "explicit ELF"):
                    PROFILE.collect(output, [str(binary), "--seconds", "3"])
                self.assertFalse(output.exists())
                launch.assert_not_called()

    def test_collector_failure_preserves_final_manifest_and_pid(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            binary, elf, output = base / "night100", base / "ELF", base / "fresh"
            binary.write_text("fake executable"); binary.chmod(0o755); elf.write_text("fake ELF")
            class Child:
                pid = 777
                def wait(self, timeout):
                    raise subprocess.TimeoutExpired("night100", timeout)
            with patch.object(PROFILE.sys, "platform", "darwin"), \
                    patch.object(PROFILE.subprocess, "check_output", return_value=""), \
                    patch.object(PROFILE.subprocess, "Popen", return_value=Child()):
                with self.assertRaisesRegex(RuntimeError, "did not finish"):
                    PROFILE.collect(output, [str(binary), str(elf), "--seconds", "3"])
                result = json.loads((output / "result.json").read_text())
                self.assertEqual(result["timeout_pid"], 777)
                self.assertIn("not terminated", result["error"])

    def test_sampler_attaches_only_to_fresh_child(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            binary, elf, output = base / "night100", base / "ELF", base / "fresh"
            binary.write_text("fake executable"); binary.chmod(0o755); elf.write_text("fake ELF")
            calls = []
            class Child:
                pid = 123
                def poll(self): return None
                def wait(self, timeout): return 0
            def launch(command, **kwargs):
                calls.append(command)
                if command[0] == "/usr/bin/sample":
                    self.assertEqual(command[1], "123")
                    Path(command[-1]).write_text(SAMPLE)
                return Child()
            with patch.object(PROFILE.sys, "platform", "darwin"), \
                    patch.object(PROFILE.subprocess, "check_output", return_value=""), \
                    patch.object(PROFILE.subprocess, "Popen", side_effect=launch), \
                    patch.object(PROFILE.time, "sleep"):
                result = PROFILE.collect(output, [str(binary), str(elf), "--seconds", "5"], 2)
                self.assertTrue(result["sampled"])
                self.assertEqual(len(calls), 2)
                self.assertEqual(json.loads((output / "sample.json").read_text())["gameThreadSamples"], 10)

    def test_cli_analyze(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "sample.txt"
            path.write_text(SAMPLE)
            result = subprocess.run([sys.executable, str(ROOT / "tools/profile_cpu.py"), "analyze", str(path)],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)["gameThreadSamples"], 10)


if __name__ == "__main__":
    unittest.main()
