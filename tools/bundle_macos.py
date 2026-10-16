#!/usr/bin/env python3
"""Copy and relocate a macOS executable's non-system dylib closure."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def command(arguments):
    return subprocess.run([str(a) for a in arguments], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def system_path(value):
    return value.startswith('/System/Library/') or value.startswith('/usr/lib/')


def dependencies(text):
    """otool -L includes a dylib's own install ID; caller removes it."""
    return [line.strip().split(' (compatibility version ', 1)[0]
            for line in text.splitlines()[1:] if line.startswith(('\t', ' '))]


def rpaths(text):
    result = []
    active = False
    for line in text.splitlines():
        if line.strip().startswith('cmd '):
            active = line.strip() == 'cmd LC_RPATH'
        elif active:
            match = re.match(r'\s*path (.+) \(offset \d+\)', line)
            if match:
                result.append(match[1])
                active = False
    return result


def minimum_macos(text):
    """Read the actual Mach-O deployment target, including older load commands."""
    versions = []
    for block in re.split(r'(?m)^Load command \d+\s*$', text):
        if re.search(r'\bcmd LC_BUILD_VERSION\b', block):
            platform = re.search(r'(?m)^\s*platform\s+(\S+)', block)
            if platform and platform[1] not in ('1', 'MACOS', 'macos'):
                continue
            match = re.search(r'(?m)^\s*minos\s+(\d+(?:\.\d+){0,2})\s*$', block)
        elif re.search(r'\bcmd LC_VERSION_MIN_MACOSX\b', block):
            match = re.search(r'(?m)^\s*version\s+(\d+(?:\.\d+){0,2})\s*$', block)
        else:
            continue
        if match:
            versions.append(match[1])
    return max(versions, key=lambda version: tuple(map(int, version.split('.')))) if versions else None


def expand(value, owner, executable):
    return value.replace('@loader_path', str(owner.parent)).replace(
        '@executable_path', str(executable.parent))


def resolve(value, owner, executable, search):
    expanded = expand(value, owner, executable)
    candidates = ([Path(directory) / expanded[len('@rpath/'):]
                   for directory in search] if expanded.startswith('@rpath/')
                  else [Path(expanded)])
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise ValueError(f'Cannot resolve dependency {value!r} of {owner}')


def audit(binary, run=command):
    binary = Path(binary).resolve(strict=True)
    pending = [(binary, ())]
    nodes = {}
    names = {}
    while pending:
        path, inherited = pending.pop()
        if path in nodes:
            continue
        ids = run(['otool', '-D', path]).splitlines()[1:]
        identity = ids[0].strip() if ids else None
        load_commands = run(['otool', '-l', path])
        paths = rpaths(load_commands)
        search = tuple(expand(p, path, binary) for p in paths) + inherited
        refs = dependencies(run(['otool', '-L', path]))
        refs = [ref for ref in refs if ref != identity]
        edges = []
        for ref in refs:
            if system_path(ref):
                edges.append((ref, None))
            else:
                target = resolve(ref, path, binary, search)
                if target == binary:
                    raise ValueError(f'Dylib dependency unexpectedly points to executable: {ref}')
                old = names.setdefault(target.name.casefold(), target)
                if old != target:
                    raise ValueError(f'Dylib filename collision: {old} and {target}')
                edges.append((ref, target))
                pending.append((target, search))
        nodes[path] = {'id': identity, 'rpaths': paths, 'edges': edges,
                       'minimum_macos': minimum_macos(load_commands)}
    return binary, nodes


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def package_root(path):
    """Homebrew receipts preserve version and dependency provenance."""
    for parent in path.parents:
        if (parent / 'INSTALL_RECEIPT.json').is_file():
            return parent
    return None


def collect_licenses(paths, output):
    packages = {}
    for source in paths:
        root = package_root(source)
        if root is None or root in packages:
            continue
        copied = []
        label = root.parent.name + '-' + root.name
        destination = output / label
        candidates = set()
        # Root license texts and the package's own shared license/doc directories.
        for directory in [root, root / 'share/licenses', root / 'share/doc']:
            if not directory.is_dir():
                continue
            for candidate in directory.glob('*'):
                if candidate.is_dir() and directory != root:
                    candidates.update(candidate.glob('*'))
                else:
                    candidates.add(candidate)
        for candidate in sorted(candidates):
            name = candidate.name.upper()
            if (candidate.is_file() and candidate.stat().st_size <= 2 * 1024 * 1024
                    and (name.startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE'))
                         or candidate.name == 'INSTALL_RECEIPT.json')):
                destination.mkdir(parents=True, exist_ok=True)
                # Keep relative paths to avoid silently overwriting duplicate notices.
                target = destination / candidate.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(candidate, target)
                copied.append(str(target.relative_to(output.parent)))
        packages[root] = {'name': root.parent.name, 'version': root.name,
                          'source': str(root), 'notices': copied,
                          'license_text_found': any(not p.endswith('INSTALL_RECEIPT.json') for p in copied)}
    return list(packages.values())


def collect_linked_notices(output):
    root = Path(__file__).resolve().parents[1]
    candidates = [('PS2Recomp', root / 'vendor/PS2Recomp/LICENSE'),
                  ('raylib', root / 'build/toolchain/_deps/raylib-src/LICENSE'),
                  ('GLFW', root / 'build/toolchain/_deps/raylib-src/src/external/glfw/LICENSE.md')]
    records = []
    for name, path in candidates:
        if path.is_file():
            target = output / name / path.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            records.append({'name': name, 'source': str(path), 'sha256': digest(path),
                            'notice': str(target.relative_to(output.parent))})
    return records


def verify(binary, libraries, run=command):
    """All non-system load commands must resolve inside this output directory."""
    root = binary.parent.resolve()
    for path in [binary, *libraries]:
        ids = run(['otool', '-D', path]).splitlines()[1:]
        identity = ids[0].strip() if ids else None
        for ref in dependencies(run(['otool', '-L', path])):
            if ref == identity or system_path(ref):
                continue
            if not ref.startswith(('@loader_path/', '@executable_path/')):
                raise ValueError(f'External dependency remains in {path}: {ref}')
            target = Path(expand(ref, path, binary)).resolve(strict=True)
            if not target.is_relative_to(root):
                raise ValueError(f'Dependency escapes package: {path}: {ref}')
        if rpaths(run(['otool', '-l', path])):
            raise ValueError(f'Unnecessary source rpath remains in {path}')


def bundle(binary, output, strip=True, run=command):
    binary, nodes = audit(binary, run)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    names = ['night100', 'lib', 'runtime-licenses', 'runtime-dependencies.json']
    for name in names:
        if (output / name).exists():
            raise ValueError(f'Refusing to overwrite runtime package output: {output / name}')
    stage = Path(tempfile.mkdtemp(prefix='.runtime-', dir=output))
    try:
        (stage / 'lib').mkdir()
        (stage / 'runtime-licenses').mkdir()
        copied = {}
        for source in nodes:
            destination = stage / ('night100' if source == binary else 'lib/' + source.name)
            shutil.copy2(source, destination)
            destination.chmod(destination.stat().st_mode | 0o200)
            copied[source] = destination
        for source, node in nodes.items():
            destination = copied[source]
            arguments = ['install_name_tool']
            if source != binary:
                arguments += ['-id', '@loader_path/' + source.name]
            for old, target in node['edges']:
                if target is not None:
                    new = ('@executable_path/lib/' if source == binary else '@loader_path/') + target.name
                    arguments += ['-change', old, new]
            for old in dict.fromkeys(node['rpaths']):
                arguments += ['-delete_rpath', old]
            if len(arguments) > 1:
                run([*arguments, destination])
        if strip:
            run(['strip', '-S', stage / 'night100'])
        libraries = [p for source, p in copied.items() if source != binary]
        verify(stage / 'night100', libraries, run)
        # Each copied Mach-O is an independent code object; sign libraries first,
        # then the executable, after every load-command mutation is complete.
        for path in [*sorted(libraries), stage / 'night100']:
            run(['codesign', '--force', '--sign', '-', '--timestamp=none', path])
            run(['codesign', '--verify', '--strict', path])
        packages = collect_licenses(nodes, stage / 'runtime-licenses')
        linked_notices = collect_linked_notices(stage / 'runtime-licenses')
        records = []
        supported_architectures = None
        for source, destination in copied.items():
            architectures = sorted(set(run(['lipo', '-archs', source]).split()))
            if not architectures:
                raise ValueError(f'Cannot determine Mach-O architecture: {source}')
            supported_architectures = (set(architectures) if supported_architectures is None
                                       else supported_architectures.intersection(architectures))
            records.append({'source': str(source), 'source_sha256': digest(source),
                            'path': str(destination.relative_to(stage)),
                            'bytes': destination.stat().st_size, 'sha256': digest(destination),
                            'architectures': architectures,
                            'minimum_macos': nodes[source]['minimum_macos'],
                            'dependencies': [ref for ref, _ in nodes[source]['edges']]})
        if not supported_architectures:
            raise ValueError('Executable and dylibs have no common Mach-O architecture')
        versions = [node['minimum_macos'] for node in nodes.values() if node['minimum_macos']]
        minimum = max(versions, key=lambda version: tuple(map(int, version.split('.')))) if versions else None
        manifest = {'schema': 1, 'executable': 'night100', 'libraries': len(libraries),
                    'architectures': sorted(supported_architectures),
                    'architecture': ','.join(sorted(supported_architectures)),
                    'minimum_macos': minimum,
                    'deployment_targets_complete': len(versions) == len(nodes),
                    'source_runtime_bytes': sum(p.stat().st_size for p in nodes),
                    'bundled_runtime_bytes': sum(p.stat().st_size for p in copied.values()),
                    'debug_symbols_stripped': strip, 'files': records, 'packages': packages,
                    'linked_notices': linked_notices,
                    'license_note': 'Collected installed notices and Homebrew receipts; this manifest is not a complete source-code distribution.'}
        (stage / 'runtime-dependencies.json').write_text(json.dumps(manifest, indent=2) + '\n')
        for name in names:
            os.rename(stage / name, output / name)
        return manifest
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--audit-only', action='store_true')
    parser.add_argument('--keep-debug-symbols', action='store_true')
    args = parser.parse_args()
    if args.audit_only:
        binary, nodes = audit(args.binary)
        print(json.dumps({'executable': str(binary), 'libraries': len(nodes) - 1,
                          'bytes': sum(p.stat().st_size for p in nodes),
                          'files': [str(p) for p in nodes]}, indent=2))
    else:
        if args.output is None:
            parser.error('--output is required unless --audit-only is used')
        result = bundle(args.binary, args.output, not args.keep_debug_symbols)
        print(json.dumps({k: result[k] for k in ['libraries', 'source_runtime_bytes', 'bundled_runtime_bytes']}))


if __name__ == '__main__':
    main()
