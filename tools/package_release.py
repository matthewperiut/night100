#!/usr/bin/env python3
"""Assemble a relocatable local release without copying the development tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from package_resources import publish

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def package(iso, binary, output):
    iso, binary, output = iso.resolve(), binary.resolve(), output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f'Output already exists; choose a new folder: {output}')
    if not iso.is_file() or not binary.is_file():
        raise ValueError('Packaging requires an existing ISO and built executable')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Build beside the destination so the final rename cannot cross filesystems.
    with tempfile.TemporaryDirectory(prefix=f'.{output.name}-package-', dir=output.parent) as temporary:
        stage = Path(temporary)/'release'
        subprocess.run([sys.executable, str(ROOT/'tools/package_resources.py'),
                        '--iso', str(iso), '--output', str(stage)], check=True)
        subprocess.run([sys.executable, str(ROOT/'tools/controller_assets.py'), '--game', str(stage/'game'),
                        '--output', str(stage/'controllers')], check=True)
        subprocess.run([sys.executable, str(ROOT/'tools/bundle_macos.py'),
                        '--binary', str(binary), '--output', str(stage)], check=True)
        runtime = json.loads((stage/'runtime-dependencies.json').read_text())
        platform = f"macOS {runtime['minimum_macos']} or later, ARM64" if runtime.get('minimum_macos') else 'macOS ARM64'
        provenance = stage/'source-provenance'
        provenance.mkdir()
        for name in ('dependencies.json', 'CMakeLists.txt', 'night100'):
            shutil.copyfile(ROOT/name, provenance/name)
        shutil.copytree(ROOT/'patches', provenance/'patches')
        shutil.copytree(ROOT/'licenses', stage/'project-licenses')
        (stage/'Launch.command').write_text(
            '#!/bin/sh\nset -eu\ncd -- "$(dirname -- "$0")"\nexec ./night100 "$@"\n')
        (stage/'Launch.command').chmod(0o755)
        (stage/'README.txt').write_text(
            f'Night100 — local {platform} build\n\n'
            'Double-click Launch.command, or run ./night100 from this folder.\n'
            'The game/, controllers/ and lib/ folders must stay beside the executable.\n'
            'The original ISO and development workspace are not needed to run.\n'
            'Movies play by default. Use ./night100 --skip-videos to skip them.\n'
            'Movement: WASD. Jump/confirm: Space. Circle: C. Start: Enter.\n'
            'Camera: IJKL. Frame timings: F3. Close the window to quit.\n'
            'Use --controller-prompts sw to preview Switch button artwork.\n'
            'F11 starts a state recording; F12 saves it; F10 selects a replay.\n'
            'Recordings and memory-card saves are created locally during play.\n\n'
            'All disc movies are included with their original encoding.\n'
            'Full-game completion is unverified.\n'
            'This folder contains locally extracted copyrighted game resources.\n'
            'Rebuild instructions and source changes remain in the development project.\n')
        try:
            revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        except subprocess.CalledProcessError:
            revision = None
        sources = [ROOT/'CMakeLists.txt', ROOT/'night100', ROOT/'dependencies.json']
        for directory in ('src', 'shaders', 'tools', 'patches'):
            sources.extend(path for path in (ROOT/directory).rglob('*')
                           if path.is_file() and '__pycache__' not in path.parts)
        sources.append(ROOT/'controllers/prompts.map')
        sources.extend(path for path in (ROOT/'controllers/sw').iterdir() if path.is_file())
        # No timestamps or absolute build paths: resource output is deterministic.
        manifest = {
            'format': 'night100-local-release-v1', 'source_revision': revision,
            'architecture': 'macOS-arm64', 'minimum_macos': runtime.get('minimum_macos'),
            'iso_bytes': iso.stat().st_size,
            'resource_manifest_sha256': digest(stage/'resource-manifest.json'),
            'source_sha256': {str(path.relative_to(ROOT)): digest(path) for path in sorted(sources)},
            'files': {str(path.relative_to(stage)): (
                          {'symlink': os.readlink(path)} if path.is_symlink()
                          else {'bytes': path.stat().st_size, 'sha256': digest(path)})
                      for path in sorted(stage.rglob('*')) if path.is_file()},
        }
        (stage/'release-manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True)+'\n')
        total = sum(path.lstat().st_size for path in stage.rglob('*') if path.is_file())
        # Refuse to replace an output created while the expensive work ran.
        if output.exists() or output.is_symlink():
            raise ValueError(f'Output appeared during packaging: {output}')
        publish(stage, output)
    print(f'Release: {output}\n{total:,} bytes ({100*(1-total/iso.stat().st_size):.1f}% smaller than ISO)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', required=True, type=Path)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        package(args.iso, args.binary, args.output)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Packaging failed: {error}\n')
