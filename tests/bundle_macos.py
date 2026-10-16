#!/usr/bin/env python3
"""Host-independent fixture tests for macOS runtime dependency packaging."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('bundler', Path(__file__).parents[1] / 'tools/bundle_macos.py')
bundler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bundler)


class FakeTools:
    def __init__(self, descriptions):
        self.descriptions = descriptions
        self.changed = {}
        self.calls = []

    def __call__(self, args):
        args = [str(a) for a in args]
        self.calls.append(args)
        if args[0] in ('otool', 'install_name_tool'):
            path = Path(args[-1])
            node = self.changed.get(path)
            if node is None:
                node = dict(self.descriptions[path.read_text().splitlines()[0]])
                node['deps'] = list(node.get('deps', []))
                node['rpaths'] = list(node.get('rpaths', []))
                self.changed[path] = node
            if args[0] == 'otool':
                if args[1] == '-D':
                    return str(path) + ':\n' + (node.get('id', '') + '\n' if node.get('id') else '')
                if args[1] == '-L':
                    deps = ([node['id']] if node.get('id') else []) + node['deps']
                    return str(path) + ':\n' + ''.join('\t' + d + ' (compatibility version 1.0.0, current version 1.0.0)\n' for d in deps)
                return ('Load command 0\n cmd LC_BUILD_VERSION\n platform 1\n minos 13.3\n sdk 27.0\n' +
                        ''.join('Load command 1\n          cmd LC_RPATH\n      cmdsize 40\n         path ' + p + ' (offset 12)\n' for p in node['rpaths']))
            i = 1
            while i < len(args) - 1:
                if args[i] == '-id':
                    node['id'] = args[i + 1]
                    i += 2
                elif args[i] == '-change':
                    old, new = args[i + 1:i + 3]
                    node['deps'] = [new if dep == old else dep for dep in node['deps']]
                    i += 3
                elif args[i] == '-delete_rpath':
                    node['rpaths'].remove(args[i + 1])
                    i += 2
                else:
                    raise AssertionError(args)
        elif args[0] == 'strip':
            with Path(args[-1]).open('a') as stream:
                stream.write('\nstripped')
        elif args[0] == 'lipo':
            return 'arm64\n'
        elif args[0] != 'codesign':
            raise AssertionError(args)
        return ''


class BundlerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.binary = self.root / 'app'
        self.a = self.root / 'liba.dylib'
        self.b = self.root / 'libb.dylib'
        for path, label in [(self.binary, 'app'), (self.a, 'a'), (self.b, 'b')]:
            path.write_text(label)
        self.tools = FakeTools({'app': {'deps': [str(self.a)], 'rpaths': [str(self.root)]},
                                'a': {'id': str(self.a), 'deps': ['@rpath/libb.dylib']},
                                'b': {'id': str(self.b), 'deps': ['/usr/lib/libSystem.B.dylib']}})

    def tearDown(self):
        self.temp.cleanup()

    def test_parses_load_commands(self):
        text = 'app:\n\t@rpath/liba.dylib (compatibility version 1.0.0, current version 2.0.0)\n'
        self.assertEqual(bundler.dependencies(text), ['@rpath/liba.dylib'])
        self.assertEqual(bundler.rpaths('cmd LC_RPATH\n path /a path/lib (offset 12)\n'), ['/a path/lib'])

    def test_system_allowlist_is_narrow(self):
        self.assertTrue(bundler.system_path('/usr/lib/libz.dylib'))
        self.assertFalse(bundler.system_path('/usr/local/lib/libz.dylib'))
        self.assertFalse(bundler.system_path('/opt/homebrew/lib/libz.dylib'))

    def test_deployment_target_parser(self):
        self.assertEqual(bundler.minimum_macos('Load command 0\n cmd LC_BUILD_VERSION\n platform 1\n minos 14.2\n sdk 27.0\n'), '14.2')
        self.assertEqual(bundler.minimum_macos('Load command 0\n cmd LC_VERSION_MIN_MACOSX\n version 10.15\n sdk 13.0\n'), '10.15')
        self.assertEqual(bundler.minimum_macos('Load command 0\n cmd LC_BUILD_VERSION\n platform 1\n minos 9.3\nLoad command 1\n cmd LC_BUILD_VERSION\n platform 1\n minos 10.2.1\n'), '10.2.1')
        self.assertIsNone(bundler.minimum_macos('Load command 0\n cmd LC_BUILD_VERSION\n platform 2\n minos 14.0\n'))
        self.assertIsNone(bundler.minimum_macos('Load command 0\n cmd LC_RPATH\n'))

    def test_recursive_rpath_graph_excludes_self_id(self):
        _, graph = bundler.audit(self.binary, self.tools)
        self.assertEqual(set(graph), {self.binary, self.a, self.b})
        self.assertEqual(graph[self.a]['edges'], [('@rpath/libb.dylib', self.b)])

    def test_symlink_alias_deduplicated(self):
        alias = self.root / 'alias.dylib'
        alias.symlink_to(self.a)
        self.tools.descriptions['app']['deps'].append(str(alias))
        _, graph = bundler.audit(self.binary, self.tools)
        self.assertEqual(len(graph), 3)

    def test_missing_dependency_fails(self):
        self.tools.descriptions['app']['deps'] = ['@rpath/missing.dylib']
        with self.assertRaisesRegex(ValueError, 'Cannot resolve'):
            bundler.audit(self.binary, self.tools)

    def test_name_collision_rejected(self):
        other = self.root / 'other/liba.dylib'
        other.parent.mkdir()
        other.write_text('b')
        self.tools.descriptions['app']['deps'].append(str(other))
        with self.assertRaisesRegex(ValueError, 'filename collision'):
            bundler.audit(self.binary, self.tools)

    def test_package_relocates_and_preserves_original(self):
        output = self.root / 'out'
        output.mkdir()
        (output / 'game').mkdir()
        before = {p: p.read_bytes() for p in [self.binary, self.a, self.b]}
        result = bundler.bundle(self.binary, output, run=self.tools)
        self.assertEqual(result['libraries'], 2)
        self.assertEqual(result['minimum_macos'], '13.3')
        self.assertEqual(result['architecture'], 'arm64')
        self.assertTrue(result['deployment_targets_complete'])
        self.assertEqual(before, {p: p.read_bytes() for p in before})
        self.assertTrue((output / 'game').is_dir())
        install = [call for call in self.tools.calls if call[0] == 'install_name_tool']
        self.assertTrue(any('@executable_path/lib/liba.dylib' in call for call in install))
        self.assertTrue(any('@loader_path/libb.dylib' in call for call in install))
        sign = [Path(call[-1]).name for call in self.tools.calls if call[:2] == ['codesign', '--force']]
        self.assertEqual(sign, ['liba.dylib', 'libb.dylib', 'night100'])
        self.assertEqual(result['files'][0]['source_sha256'], bundler.digest(self.binary))

    def test_refuses_overwrite(self):
        output = self.root / 'out'
        output.mkdir()
        (output / 'lib').mkdir()
        with self.assertRaisesRegex(ValueError, 'overwrite'):
            bundler.bundle(self.binary, output, run=self.tools)

    def test_failed_stage_preserves_existing_assets(self):
        output = self.root / 'out'
        output.mkdir()
        (output / 'assets').write_text('keep')
        def failure(args):
            if args[0] == 'install_name_tool':
                raise RuntimeError('fixture failure')
            return self.tools(args)
        with self.assertRaisesRegex(RuntimeError, 'fixture failure'):
            bundler.bundle(self.binary, output, run=failure)
        self.assertEqual(list(output.iterdir()), [output / 'assets'])

    def test_verifier_rejects_external_load_command(self):
        with self.assertRaisesRegex(ValueError, 'External dependency'):
            bundler.verify(self.binary, [self.a, self.b], self.tools)

    def test_collects_notices_and_package_receipt(self):
        package = self.root / 'Cellar/foo/1.0'
        (package / 'lib').mkdir(parents=True)
        (package / 'INSTALL_RECEIPT.json').write_text('{}')
        (package / 'COPYING').write_text('license')
        library = package / 'lib/foo.dylib'
        library.write_text('foo')
        output = self.root / 'notices'
        result = bundler.collect_licenses([library], output)
        self.assertTrue(result[0]['license_text_found'])
        self.assertTrue((output / 'foo-1.0/COPYING').is_file())


if __name__ == '__main__':
    unittest.main()
