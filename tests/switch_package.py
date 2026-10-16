#!/usr/bin/env python3
"""Synthetic Switch packaging checks; no original assets/toolchain required."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location('switch_packager', ROOT / 'tools/package_switch.py')
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def nro(label=b'first'):
    data = bytearray(256)
    data[16:20] = b'NRO0'
    struct.pack_into('<I', data, 24, len(data))
    data[128:128 + len(label)] = label
    return bytes(data)


class PackageChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.resources, self.output = self.root / 'resources', self.root / 'package'
        self.binary = self.root / 'night100.nro'
        self.binary.write_bytes(nro())
        self.contents = {
            'game/SLUS_203.49': b'synthetic supported ELF fixture',
            'game/.disc-image': b'synthetic compact disc descriptor',
            'game/MODULES/IOPRP.IMG': b'IOP boot archive fixture',
            'game/FMV/DEMO/DEMO2.PSS': b'canonical demo fixture',
            'game/Z0/Z001.HIP': b'later area fixture',
        }
        for name, data in self.contents.items():
            path = self.resources / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.manifest = {
            'schema': 1, 'format': 'N100DSC1',
            'elf': {'path': 'game/SLUS_203.49', 'sha256': sha(self.contents['game/SLUS_203.49'])},
            'files': [{'path': name, 'size': len(data), 'sha256': sha(data)} for name, data in self.contents.items()],
            'aliases': [],
        }
        self.fake_source = self.root / 'source'
        self.fake_source.mkdir()
        for name in ('dependencies.json', 'dependencies-switch-video.json'):
            (self.fake_source / name).write_text('{}\n')
        patcher = mock.patch.multiple(TOOL, ELF_SHA=self.manifest['elf']['sha256'], ROOT=self.fake_source)
        patcher.start()
        self.addCleanup(patcher.stop)

    def save_manifest(self):
        (self.resources / 'resource-manifest.json').write_text(json.dumps(self.manifest))

    def package(self):
        self.save_manifest()
        with contextlib.redirect_stdout(io.StringIO()):
            TOOL.package(self.binary, self.resources, self.output)

    def alias(self, name, canonical):
        entry = next(item for item in self.manifest['files'] if item['path'] == canonical)
        self.manifest['aliases'].append({'path': name, 'canonical': canonical,
                                         'size': entry['size'], 'sha256': entry['sha256']})

    def test_exact_identity_gate_is_required(self):
        self.manifest['elf']['sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_unknown_source_manifest_schema_rejected(self):
        self.manifest['schema'] = 99
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_supported_elf_must_be_listed_for_staging(self):
        self.manifest['files'] = [item for item in self.manifest['files'] if item['path'] != 'game/SLUS_203.49']
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_non_nro_rejected_without_output(self):
        self.binary.write_bytes(b'ordinary host binary')
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_all_staged_resource_hashes_describe_actual_bytes(self):
        self.package()
        assets = json.loads((self.output / 'asset-manifest.json').read_text())
        for item in assets['files']:
            with self.subTest(path=item['path']):
                data = (self.output / item['path']).read_bytes()
                self.assertEqual(len(data), item['bytes'])
                self.assertEqual(sha(data), item['sha256'])

    def test_aliases_preserve_boot_filename_without_duplicate_movies(self):
        self.alias('game/IOPRP.IMG', 'game/MODULES/IOPRP.IMG')
        self.alias('game/FMV/DEMO/DEMO1.PSS', 'game/FMV/DEMO/DEMO2.PSS')
        self.package()
        self.assertEqual((self.output / 'game/IOPRP.IMG').read_bytes(), self.contents['game/MODULES/IOPRP.IMG'])
        self.assertFalse((self.output / 'game/FMV/DEMO/DEMO1.PSS').exists())
        self.assertIn('FMV/DEMO/DEMO1.PSS\tFMV/DEMO/DEMO2.PSS\n', (self.output / 'game/.asset-aliases').read_text())
        self.assertFalse(any(path.is_symlink() for path in self.output.rglob('*')))

    def test_runtime_only_repackage_preserves_assets_saves_and_logs(self):
        self.package()
        for relative in ('saves/card.bin', 'logs/run-preserve.log'):
            path = self.output / relative
            path.parent.mkdir()
            path.write_bytes(b'must survive an executable update')
        retained = {path.relative_to(self.output): (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns)
                    for path in self.output.rglob('*') if path.is_file() and path.name not in
                    ('night100.nro', 'runtime-manifest.json', 'README.txt')}
        self.binary.write_bytes(nro(b'second different runtime'))
        self.package()
        for relative, snapshot in retained.items():
            path = self.output / relative
            self.assertEqual((path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns), snapshot)
        self.assertEqual((self.output / 'night100.nro').read_bytes(), self.binary.read_bytes())
        self.assertEqual(json.loads((self.output / 'runtime-manifest.json').read_text())['nro_sha256'], sha(self.binary.read_bytes()))

    def test_changed_asset_version_rejected_without_replacing_runtime(self):
        self.package()
        original = (self.output / 'night100.nro').read_bytes()
        self.manifest['notes'] = 'different resource version'
        self.binary.write_bytes(nro(b'new runtime'))
        with self.assertRaises(ValueError):
            self.package()
        self.assertEqual((self.output / 'night100.nro').read_bytes(), original)

    def test_same_size_corruption_rejected_before_output(self):
        path = self.resources / 'game/Z0/Z001.HIP'
        path.write_bytes(b'!' * len(path.read_bytes()))
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_raw_resource_paths_reject_traversal_and_tsv_injection(self):
        for name in ('../escape', '/game/escape', 'game/../escape', 'game//escape', 'game/./escape',
                     'game\\escape', 'game/sdmc:/escape', 'game/a\tb', 'game/a\nb', 'game/a\x00b'):
            with self.subTest(path=name), self.assertRaises(ValueError):
                TOOL.contained(name)

    def test_source_file_symlink_cannot_import_outside_data(self):
        path = self.resources / 'game/Z0/Z001.HIP'
        outside = self.root / 'outside-secret'
        outside.write_bytes(self.contents['game/Z0/Z001.HIP'])
        path.unlink()
        path.symlink_to(outside)
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_source_directory_symlink_cannot_import_outside_data(self):
        directory = self.resources / 'game/Z0'
        outside = self.root / 'outside-directory'
        directory.rename(outside)
        directory.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_duplicate_or_case_colliding_manifest_names_rejected(self):
        entry = next(item for item in self.manifest['files'] if item['path'] == 'game/Z0/Z001.HIP')
        self.manifest['files'].append(dict(entry, path='game/z0/z001.hip'))
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_alias_must_target_listed_canonical_file_with_matching_metadata(self):
        self.alias('game/FMV/DEMO/DEMO1.PSS', 'game/FMV/DEMO/DEMO2.PSS')
        self.manifest['aliases'][0]['sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_alias_cannot_overwrite_generated_alias_table(self):
        self.alias('game/.asset-aliases', 'game/MODULES/IOPRP.IMG')
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_alias_cannot_target_another_alias(self):
        self.alias('game/FMV/DEMO/DEMO1.PSS', 'game/FMV/DEMO/DEMO2.PSS')
        self.alias('game/FMV/DEMO/DEMO3.PSS', 'game/FMV/DEMO/DEMO2.PSS')
        self.manifest['aliases'][1]['canonical'] = 'game/FMV/DEMO/DEMO1.PSS'
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_update_does_not_follow_predictable_temporary_symlinks(self):
        self.package()
        outside = self.root / 'outside-sentinel'
        outside.write_bytes(b'preserve outside file')
        (self.output / 'night100.nro.new').symlink_to(outside)
        self.binary.write_bytes(nro(b'new runtime'))
        self.package()
        self.assertEqual(outside.read_bytes(), b'preserve outside file')
        self.assertFalse((self.output / 'night100.nro').is_symlink())


if __name__ == '__main__':
    unittest.main()
