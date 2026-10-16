#!/usr/bin/env python3
"""Fast evidence-parser fixtures; the full movie route is explicitly invoked."""
import importlib.util
import hashlib
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('attract_video_tool', Path(__file__).resolve().parents[1] / 'tools' / 'test_attract_videos.py')
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


def fixture(sequence=(4, 3)):
    messages = ['[attract-test] menu responsive after 0 movie returns; last-demo=0 guest frames=60']
    for index, number in enumerate(sequence, 1):
        messages.extend([f'[movie] starting demo{number}.pss skip-mask=0x0',
                         f'[movie] finished demo{number}.pss decoded=200 displayed=200 dropped=0 audio-callbacks=100 callback-source-frames=800',
                         f'[attract-test] menu responsive after {index} movie returns; last-demo={number} guest frames=60'])
    messages.append(f'[attract-test] completed attract sequence of {len(sequence)} movies and returned to menu')
    return {'sequence': list(sequence), 'exit_code': 0, 'stalled': False,
            'events': [{'seconds': index * 80, 'message': message} for index, message in enumerate(messages)],
            'captures': [{'path': 'menu.png', 'exists': True, 'hash_matches': True}, {'path': 'video.png', 'exists': True, 'hash_matches': True}]}


class AttractEvidenceChecks(unittest.TestCase):
    def test_timeout_must_be_finite_and_positive(self):
        for value in ['nan', 'inf', '-inf', '0', '-1']:
            with mock.patch('sys.argv', ['test_attract_videos.py', '--output', 'unused', f'--timeout={value}']), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as rejected:
                    TOOL.main()
                self.assertEqual(rejected.exception.code, 2)

    def test_valid_nonsequential_names_and_lowercase(self):
        report = TOOL.validate(fixture())
        self.assertTrue(report['passed'])
        self.assertEqual(report['finished'], [4, 3])
        self.assertEqual(report['clips'][1]['counters']['callback-source-frames'], 800)

    def test_wrong_order(self):
        report = fixture()
        report['sequence'] = [3, 4]
        self.assertFalse(TOOL.validate(report)['passed'])

    def test_skip(self):
        report = fixture()
        report['events'][2]['message'] = report['events'][2]['message'].replace('finished', 'skipped')
        self.assertFalse(TOOL.validate(report)['passed'])

    def test_missing_menu_return(self):
        report = fixture()
        del report['events'][3]
        self.assertFalse(TOOL.validate(report)['passed'])

    def test_menu_entries_must_occur_between_movies(self):
        report = fixture()
        report['events'][3], report['events'][4] = report['events'][4], report['events'][3]
        self.assertFalse(TOOL.validate(report)['passed'])

    def test_sixty_guest_frames_required(self):
        report = fixture()
        report['events'][3]['message'] = report['events'][3]['message'].replace('frames=60', 'frames=1')
        self.assertFalse(TOOL.validate(report)['passed'])

    def test_missing_capture(self):
        report = fixture()
        report['captures'][1]['exists'] = False
        self.assertFalse(TOOL.validate(report)['passed'])

    def test_changed_capture_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.png'
            path.write_bytes(b'original capture bytes')
            report = fixture()
            report['captures'] = [{'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}]
            self.assertTrue(TOOL.validate(TOOL.audit_captures(report))['passed'])
            path.write_bytes(b'changed capture bytes')
            self.assertFalse(TOOL.validate(TOOL.audit_captures(report))['passed'])
            path.unlink()
            self.assertFalse(TOOL.validate(TOOL.audit_captures(report))['passed'])

    def test_failed_or_stalled_process(self):
        for key, value in [('exit_code', 1), ('stalled', True)]:
            report = fixture()
            report[key] = value
            self.assertFalse(TOOL.validate(report)['passed'])

    def test_early_finish_fails_original_duration(self):
        report = fixture()
        report['assets'] = [{'name': 'DEMO4.PSS', 'duration_seconds': 100}, {'name': 'DEMO3.PSS', 'duration_seconds': 100}]
        self.assertFalse(TOOL.validate(report)['passed'])

    def test_original_full_route_log_format(self):
        report = fixture(tuple(range(1, 16)))
        for event in report['events']:
            event['message'] = event['message'].replace('movie returns', 'completed demos')
        report['events'][-1]['message'] = '[attract-test] completed all 15 demos and returned to menu'
        self.assertTrue(TOOL.validate(report)['passed'])


if __name__ == '__main__':
    unittest.main()
