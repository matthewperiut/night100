#!/usr/bin/env python3
"""Offline deployment/transfer-selection checks using an in-memory HTTP client."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import urllib.error
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import switch_remote
SPEC = importlib.util.spec_from_file_location('switch_deployer', ROOT / 'tools/switch_deploy.py')
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class MemoryClient:
    """Models observable HTTP behavior without reading credentials or networking."""
    def __init__(self):
        self.files, self.calls = {}, []
        self.corrupt_uploads = False

    def status(self):
        self.calls.append(('status',))
        return {'connected': True}

    def read(self, path):
        self.calls.append(('read', path))
        if path not in self.files and any(name.startswith(path.rstrip('/')+'/') for name in self.files):
            return b'{"entries":[]}'
        if path not in self.files:
            raise urllib.error.HTTPError('http://fixture/files', 404, 'missing', {}, None)
        return self.files[path]

    def hash(self, path):
        self.calls.append(('hash', path))
        if path not in self.files:
            raise urllib.error.HTTPError('http://fixture/files/hash', 404, 'missing', {}, None)
        return {'hash': sha(self.files[path]), 'size': len(self.files[path])}

    def upload(self, source, destination):
        self.calls.append(('upload', destination))
        data = Path(source).read_bytes()
        self.files[destination] = b'corrupted transfer' if self.corrupt_uploads else data

    def tap(self, *buttons):
        self.calls.append(('tap', *buttons))

    def screenshot(self, output):
        self.calls.append(('screenshot', str(output)))
        Path(output).write_bytes(b'synthetic screenshot')

    def ip(self):
        return '192.0.2.1'


class DeploymentChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / 'package'
        self.package.mkdir()
        self.remote_root = '/switch/night100'
        self.client = MemoryClient()
        binary = bytearray(256)
        binary[16:20] = b'NRO0'
        self.binary = self.package / 'night100.nro'
        self.binary.write_bytes(binary)
        self.data = {'game/SLUS_203.49': b'synthetic executable fixture',
                     'game/MN/MNU1.HIP': b'synthetic menu fixture',
                     'game/Z0/Z001.HIP': b'synthetic later area fixture'}
        self.manifest = {'schema': 1, 'format': 'night100-switch-assets-v1',
                         'elf_sha256': 'd5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1',
                         'files': []}
        for name, data in self.data.items():
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            self.manifest['files'].append({'path': name, 'bytes': len(data), 'sha256': sha(data),
                                           'bootstrap': not name.startswith('game/Z0/')})
        self.save_manifest()

    def save_manifest(self):
        (self.package / 'asset-manifest.json').write_text(json.dumps(self.manifest))

    def assets(self, selection):
        self.save_manifest()
        with contextlib.redirect_stdout(io.StringIO()):
            return TOOL.deploy_assets(self.client, self.package, self.remote_root, selection)

    def cli(self, *arguments):
        with mock.patch.object(TOOL, 'Client', return_value=self.client), \
             mock.patch('sys.argv', ['switch_deploy.py', '--package', str(self.package), *arguments]), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return TOOL.main()

    def test_bootstrap_uploads_only_required_subset_then_all_resumes(self):
        first = self.assets('bootstrap')
        self.assertEqual(first['uploaded'], 2)
        self.assertFalse(first['complete'])
        self.assertNotIn('/switch/night100/game/Z0/Z001.HIP', self.client.files)
        self.client.calls.clear()
        second = self.assets('all')
        self.assertEqual(second['uploaded'], 1)
        self.assertTrue(second['complete'])
        self.assertEqual(second['uploaded_bytes'], len(self.data['game/Z0/Z001.HIP']))
        data_uploads = [path for action, *rest in self.client.calls if action == 'upload'
                        for path in rest if '/game/' in path]
        self.assertEqual(data_uploads, ['/switch/night100/game/Z0/Z001.HIP'])

    def test_unchanged_deployment_does_not_retransfer_assets(self):
        self.assets('all')
        self.client.calls.clear()
        result = self.assets('all')
        self.assertEqual(result['uploaded_bytes'], 0)
        self.assertEqual(result['uploaded'], 0)
        self.assertTrue(result['complete'])
        self.assertFalse(any(call[0] == 'upload' and '/game/' in call[1] for call in self.client.calls))

    def test_default_nro_only_update_preserves_resources_saves_and_logs(self):
        retained = {'/switch/night100/game/keep.HIP': b'game data',
                    '/switch/night100/saves/card.bin': b'saved progress',
                    '/switch/night100/logs/run-old.log': b'prior evidence'}
        self.client.files.update(retained)
        self.assertEqual(self.cli(), 0)
        self.assertEqual([call[1] for call in self.client.calls if call[0] == 'upload'], ['/switch/night100/night100.nro'])
        for path, data in retained.items():
            self.assertEqual(self.client.files[path], data)
        self.client.calls.clear()
        self.assertEqual(self.cli(), 0)
        self.assertFalse(any(call[0] == 'upload' for call in self.client.calls))

    def test_assets_only_does_not_upload_or_launch_nro(self):
        self.assertEqual(self.cli('--assets=bootstrap', '--assets-only'), 0)
        self.assertNotIn('/switch/night100/night100.nro', self.client.files)
        self.assertFalse(any(call[0] == 'tap' for call in self.client.calls))

    def test_upload_is_verified_after_transfer(self):
        self.client.corrupt_uploads = True
        with self.assertRaises(ValueError):
            TOOL.upload_verified(self.client, self.binary, '/switch/night100/night100.nro')

    def test_http_errors_other_than_missing_are_not_hidden(self):
        denied = urllib.error.HTTPError('http://fixture/files/hash', 403, 'denied', {}, None)
        with mock.patch.object(self.client, 'hash', side_effect=denied):
            with self.assertRaises(urllib.error.HTTPError):
                TOOL.upload_verified(self.client, self.binary, '/switch/night100/night100.nro')
        self.assertFalse(any(call[0] == 'upload' for call in self.client.calls))

    def test_http_upload_streams_file_with_fixed_content_length(self):
        connection = mock.Mock()
        connection.getresponse.return_value.status = 201
        connection.getresponse.return_value.read.return_value = b'{"uploaded":true}'
        with mock.patch.dict('os.environ', {'SWITCH_TOKEN': 'synthetic-test-token'}), \
             mock.patch.object(switch_remote.http.client, 'HTTPConnection', return_value=connection):
            client = switch_remote.Client('fixture.invalid', 4150)
            client.upload(self.binary, '/switch/night100/night100.nro')
        request = connection.request.call_args
        self.assertEqual(request.args[0], 'PUT')
        self.assertTrue(hasattr(request.kwargs['body'], 'read'))
        self.assertFalse(isinstance(request.kwargs['body'], (bytes, bytearray)))
        self.assertEqual(request.kwargs['headers']['Content-Length'], str(self.binary.stat().st_size))
        connection.close.assert_called_once()

    def test_invalid_durations_rejected_before_device_io(self):
        for value in ('nan', 'inf', '-inf', '-1', '86401'):
            with self.subTest(seconds=value):
                self.client.calls.clear()
                with self.assertRaises(SystemExit) as error:
                    self.cli('--seconds=' + value)
                self.assertEqual(error.exception.code, 2)
                self.assertEqual(self.client.calls, [])

    def test_unsafe_remote_roots_and_relative_paths_rejected(self):
        for root in ('/', '/switch', 'switch/night100', '/switch/night100/../../outside',
                     '/switch//night100', '/switch/./night100', '/switch/night100\n', '/switch/sdmc:/bad'):
            with self.subTest(root=root), self.assertRaises(ValueError):
                TOOL.remote_path(root, 'night100.nro')
        for name in ('../outside', '/outside', 'game/../outside', 'game//asset', 'game/./asset',
                     'game\\asset', 'game/sdmc:/asset', 'game/a\tb'):
            with self.subTest(relative=name), self.assertRaises(ValueError):
                TOOL.remote_path(self.remote_root, name)

    def test_invalid_nro_rejected_before_upload_or_launch(self):
        self.binary.write_bytes(b'desktop executable fixture')
        with self.assertRaises((ValueError, SystemExit)):
            self.cli()
        self.assertFalse(any(call[0] in ('upload', 'tap') for call in self.client.calls))

    def test_manifest_cannot_write_savedata(self):
        self.manifest['files'][0]['path'] = 'saves/card.bin'
        with self.assertRaises(ValueError):
            self.assets('all')
        self.assertFalse(any(call[0] == 'upload' for call in self.client.calls))

    def test_preflight_rejects_late_corruption_before_partial_deploy(self):
        source = self.package / 'game/Z0/Z001.HIP'
        source.write_bytes(b'!' * len(source.read_bytes()))
        with self.assertRaises(ValueError):
            self.assets('all')
        self.assertFalse(any(call[0] == 'upload' for call in self.client.calls))

    def test_symlinked_source_cannot_import_outside_package(self):
        source = self.package / 'game/Z0/Z001.HIP'
        outside = self.root / 'outside-file'
        source.rename(outside)
        source.symlink_to(outside)
        with self.assertRaises(ValueError):
            self.assets('all')
        self.assertFalse(any(call[0] == 'upload' for call in self.client.calls))

    def launch(self, log, *extra, timeout=False):
        output = self.root / 'run-evidence'
        self.client.files['/switch/night100/logs/run-host-123.log'] = log.encode()
        with mock.patch.object(TOOL.time, 'time_ns', return_value=123), \
             mock.patch.object(TOOL.time, 'sleep'), \
             mock.patch.object(TOOL.subprocess, 'run') as process:
            if timeout:
                with mock.patch.object(TOOL.time, 'monotonic', side_effect=[0, 0, 10000]):
                    result = self.cli('--launch', '--output', str(output), '--seconds=1', *extra)
            else:
                result = self.cli('--launch', '--output', str(output), '--seconds=1', *extra)
        return result, json.loads((output / 'report.json').read_text()), process

    def test_launch_transmits_nro_once_via_netloader(self):
        result, report, process = self.launch('NIGHT100_SWITCH_FRAME_READY\nNIGHT100_SWITCH_STOP deadline\n')
        self.assertEqual(result, 0)
        self.assertTrue(report['passed'])
        process.assert_called_once()
        command = process.call_args.args[0]
        self.assertEqual(command.count(str(self.binary.resolve())), 1)
        self.assertIn('night100/night100.nro', command)
        self.assertFalse(any(call[0] == 'upload' for call in self.client.calls))
        self.assertEqual([call for call in self.client.calls if call[0] == 'tap'], [('tap', 'Y')])

    def test_fatal_runtime_marker_overrides_prior_ready_marker(self):
        result, report, _ = self.launch('NIGHT100_SWITCH_FRAME_READY\nNIGHT100_SWITCH_FATAL failed fixture\n')
        self.assertEqual(result, 1)
        self.assertFalse(report['passed'])

    def test_ready_without_clean_stop_is_not_a_success(self):
        result, report, _ = self.launch('NIGHT100_SWITCH_FRAME_READY\n', timeout=True)
        self.assertEqual(result, 1)
        self.assertFalse(report['passed'])

    def test_gpu_selftest_requires_its_own_success_marker(self):
        result, report, _ = self.launch('NIGHT100_SWITCH_GPU_TEST_OK\n', '--gpu-self-test')
        self.assertEqual(result, 0)
        self.assertTrue(report['passed'])

    def test_cpu_profile_option_reaches_the_nro(self):
        import shlex
        result, report, process = self.launch('NIGHT100_SWITCH_FRAME_READY\nNIGHT100_SWITCH_STOP deadline\n', '--cpu-profile', '--native-cpu-profile', '--cpu-kernel-benchmark')
        self.assertEqual(result, 0)
        command = process.call_args.args[0]
        self.assertIn('--cpu-profile', shlex.split(command[command.index('--args') + 1]))
        self.assertIn('--native-cpu-profile', shlex.split(command[command.index('--args') + 1]))
        self.assertIn('--cpu-kernel-benchmark', shlex.split(command[command.index('--args') + 1]))

    def test_numbered_replay_argument_reaches_nro(self):
        import shlex
        result, _, process = self.launch('NIGHT100_SWITCH_FRAME_READY\nNIGHT100_SWITCH_STOP deadline\n', '--replay-session=7')
        self.assertEqual(result, 0)
        command = process.call_args.args[0]
        arguments = shlex.split(command[command.index('--args') + 1])
        self.assertEqual(arguments[arguments.index('--replay-session')+1], '7')

    def test_invalid_replay_numbers_rejected_before_device_io(self):
        for number in ('0', '-1', 'abc', '18446744073709551616'):
            with self.subTest(number=number):
                self.client.calls.clear()
                with self.assertRaises(SystemExit):
                    self.cli('--replay-session='+number)
                self.assertEqual(self.client.calls, [])

    def test_interactive_launch_retains_live_game_after_diagnostics(self):
        result, report, _ = self.launch('NIGHT100_SWITCH_FRAME_READY\n[stage] metrics elapsed_s=15 frames=600\n', '--seconds=0')
        self.assertEqual(result, 0)
        self.assertEqual(report['status'], 'running')
        self.assertTrue((self.root/'run-evidence/settled.jpg').is_file())

    def test_interactive_launch_still_rejects_fatal_after_ready(self):
        result, report, _ = self.launch('NIGHT100_SWITCH_FRAME_READY\n[stage] metrics elapsed_s=15\nNIGHT100_SWITCH_FATAL test\n', '--seconds=0')
        self.assertEqual(result, 1)
        self.assertEqual(report['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
