#!/usr/bin/env python3
"""Stage a self-contained Switch runtime plus immutable external disc assets.

The NRO embeds shaders and authored controller artwork. Original game resources
stay beside it, so a rebuild replaces the executable without copying assets.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import re
from package_resources import safe_path, contained as resource_path, publish

ROOT = Path(__file__).resolve().parents[1]
ELF_SHA = 'd5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def contained(path):
    value = safe_path(path)
    if len(value.parts)<2 or value.parts[0] != 'game':
        raise ValueError('Invalid resource manifest path')
    return value


def bootstrap(path):
    value = PurePosixPath(path)
    if value.parts[:2] in (('game', 'MN'), ('game', 'MODULES'), ('game', '.disc-data')):
        return True
    if str(value) in ('game/H0/H001.HIP', 'game/I0/I001.HIP'):
        return True
    if value.parts[:2] == ('game', 'FMV'):
        return value.name in ('WBLOGO.PSS', 'THQLOGO.PSS', 'HILOGO.PSS', 'RW4.PSS', 'INTRO.PSS')
    return len(value.parts) == 2


def atomic_write(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.'+path.name+'-', delete=False) as stream:
        temporary=Path(stream.name);stream.write(data)
    try:os.replace(temporary,path)
    finally:temporary.unlink(missing_ok=True)


def validate_manifest(manifest, resources, verify_bytes):
    if manifest.get('schema') != 1 or manifest.get('format') != 'N100DSC1':
        raise ValueError('Unsupported source resource manifest')
    entries={};seen=set()
    for item in manifest['files']:
        relative=contained(item['path']);name=str(relative)
        if name.casefold() in seen or name.casefold()=='game/.asset-aliases':
            raise ValueError('Duplicate or reserved resource path')
        if type(item['size']) is not int or item['size']<0 or not re.fullmatch(r'[0-9a-f]{64}',item['sha256']):
            raise ValueError('Invalid resource size or digest')
        source=resource_path(resources,name)
        if verify_bytes and (not source.is_file() or source.stat().st_size!=item['size'] or digest(source)!=item['sha256']):
            raise ValueError(f'Resource checksum mismatch: {relative}')
        entries[name]=item;seen.add(name.casefold())
    elf=entries.get('game/SLUS_203.49')
    if not elf or elf['sha256']!=ELF_SHA:
        raise ValueError('Supported ELF must be listed in resource files')
    for item in manifest.get('aliases',[]):
        name=str(contained(item['path']));canonical=str(contained(item['canonical']))
        if name.casefold() in seen or name.casefold()=='game/.asset-aliases' or canonical not in entries:
            raise ValueError('Invalid, duplicate or chained resource alias')
        original=entries[canonical]
        if item['size']!=original['size'] or item['sha256']!=original['sha256']:
            raise ValueError('Alias differs from its canonical resource')
        seen.add(name.casefold())


def package(binary, resources, output):
    binary, resources, output = binary.resolve(), resources.resolve(), output.absolute()
    if output.is_symlink():raise ValueError('Switch output must not be a symlink')
    manifest_path = resources / 'resource-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('elf', {}).get('sha256') != ELF_SHA or digest(resources / 'game/SLUS_203.49') != ELF_SHA:
        raise ValueError('Switch resources require the exact supported USA executable')
    with binary.open('rb') as stream:
        stream.seek(0x10)
        if stream.read(4) != b'NRO0':
            raise ValueError('Expected an actual libnx NRO executable')
    resource_id = digest(manifest_path)
    validate_manifest(manifest,resources,verify_bytes=not output.exists())
    if output.exists():
        old = json.loads((output / 'asset-manifest.json').read_text())
        if old.get('format')!='night100-switch-assets-v1' or old.get('schema')!=1 or old['source_resource_manifest_sha256'] != resource_id:
            raise ValueError('Asset version differs; choose a new output folder')
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.night100-switch-', dir=output.parent) as temporary:
            stage = Path(temporary) / 'package'
            stage.mkdir()
            files = []
            for entry in manifest['files']:
                relative = contained(entry['path'])
                source, target = resource_path(resources,str(relative)), stage / str(relative)
                if source.stat().st_size != entry['size']:
                    raise ValueError(f'Resource size mismatch: {relative}')
                target.parent.mkdir(parents=True, exist_ok=True)
                try:
                    os.link(source, target)
                except OSError:
                    shutil.copyfile(source, target)
                files.append({'path': str(relative), 'bytes': entry['size'], 'sha256': entry['sha256'],
                              'bootstrap': bootstrap(str(relative))})
            aliases = []
            for alias in manifest.get('aliases', []):
                relative, canonical = contained(alias['path']), contained(alias['canonical'])
                aliases.append(f'{relative.relative_to("game")}\t{canonical.relative_to("game")}\n')
                # The small IOP boot archive alias is also opened directly by
                # SDK file HLE. Video aliases resolve through .asset-aliases.
                if relative.suffix.upper() != '.PSS':
                    target = stage / str(relative)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    try:
                        os.link(resources / str(canonical), target)
                    except OSError:
                        shutil.copyfile(resources / str(canonical), target)
                    files.append({'path': str(relative), 'bytes': alias['size'], 'sha256': alias['sha256'], 'bootstrap': True})
            alias_file = stage / 'game/.asset-aliases'
            alias_file.write_text(''.join(sorted(aliases)))
            files.append({'path': 'game/.asset-aliases', 'bytes': alias_file.stat().st_size,
                          'sha256': digest(alias_file), 'bootstrap': True})
            shutil.copyfile(manifest_path, stage / 'resource-manifest.json')
            assets = {'schema': 1, 'format': 'night100-switch-assets-v1', 'elf_sha256': ELF_SHA,
                      'source_resource_manifest_sha256': resource_id, 'files': sorted(files, key=lambda x: x['path'])}
            (stage / 'asset-manifest.json').write_text(json.dumps(assets, indent=2, sort_keys=True) + '\n')
            publish(stage, output)
    # Only executable and its build manifest change on subsequent packages.
    with tempfile.NamedTemporaryFile(dir=output,prefix='.night100-nro-',delete=False) as stream:
        temporary=Path(stream.name)
        with binary.open('rb') as source:shutil.copyfileobj(source,stream,1024*1024)
    try:os.replace(temporary, output / 'night100.nro')
    finally:temporary.unlink(missing_ok=True)
    sources = [ROOT / 'dependencies.json', ROOT / 'dependencies-switch-video.json']
    for directory in ('src', 'platform', 'cmake', 'shaders', 'patches'):
        sources.extend(path for path in (ROOT / directory).rglob('*') if path.is_file())
    build = {'format': 'night100-switch-runtime-v1', 'nro_sha256': digest(output / 'night100.nro'),
             'nro_bytes': binary.stat().st_size, 'asset_manifest_sha256': digest(output / 'asset-manifest.json'),
             'sources': {str(p.relative_to(ROOT)): digest(p) for p in sorted(sources)}}
    atomic_write(output / 'runtime-manifest.json', (json.dumps(build, indent=2, sort_keys=True) + '\n').encode())
    atomic_write(output / 'README.txt', (
        'Night100 Switch homebrew\n\nCopy this folder to sdmc:/switch/night100/. Launch night100.nro from hbmenu.\n'
        'The NRO contains runtime code, deko3d shaders and Switch prompts. Original disc resources stay in game/.\n'
        'After rebuilding, replace night100.nro; unchanged game assets do not need copying.\n'
        'Left stick: move. Right stick: camera. B: confirm/jump. A: back. Y/X: original Square/Triangle.\n'
        'Plus: Start. Minus: Select. L/R and ZL/ZR retain shoulder controls.\n'
        'Buffered diagnostic logs are under logs/. No per-frame file flushes are used.\n'
        'Full-game completion and 60 FPS on Switch require device validation.\n').encode())
    print(f'Switch package: {output}\nNRO: {binary.stat().st_size:,} bytes; game assets retained between builds.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/switch/night100.nro')
    parser.add_argument('--resources', type=Path, default=ROOT / 'out/night100')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/switch/night100')
    args = parser.parse_args()
    package(args.binary, args.resources, args.output)
