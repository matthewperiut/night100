#!/usr/bin/env python3
"""Build the pinned minimal Horizon NVDEC MPEG-2 FFmpeg libraries locally."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def apply_compatibility_patches(source, dependency):
    """Accept pristine or exact recipe-prefix changes; never reset user edits.

    Reconstruct expected changes in a disposable Git index before touching the
    checkout. Comparing complete binary diffs also rejects unrelated staged
    changes which are hidden by opposing working-tree edits.
    """
    source=Path(source)
    revision=dependency['commit']
    patches=[]
    receipts=[]
    for relative in dependency.get('patches', []):
        path=(ROOT / relative).resolve()
        if not path.is_relative_to(ROOT.resolve()) or not path.is_file():
            raise SystemExit('Invalid recorded FFmpeg compatibility patch: '+str(relative))
        if path in patches:
            raise SystemExit('Duplicate FFmpeg compatibility patch: '+str(relative))
        patches.append(path)
        receipts.append({'path':relative,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    def git(*arguments,env=None):
        return subprocess.check_output(['git','-C',str(source),*arguments],env=env)
    diff_options=['--no-ext-diff','--no-textconv','--binary','--full-index','--no-renames']
    def diff(cached=False,env=None):
        return git('diff',*diff_options,*(['--cached'] if cached else []),revision,'--',env=env)
    if git('ls-files','--others','--exclude-standard','-z'):
        raise SystemExit('FFmpeg checkout has unrecorded untracked files')
    with tempfile.TemporaryDirectory(prefix='night100-ffmpeg-index-') as directory:
        patch_env=os.environ.copy()
        patch_env['GIT_INDEX_FILE']=str(Path(directory)/'index')
        git('read-tree',revision,env=patch_env)
        prefixes=[diff(cached=True,env=patch_env)]
        for path in patches:
            git('apply','--cached','--whitespace=nowarn',str(path),env=patch_env)
            prefixes.append(diff(cached=True,env=patch_env))
    actual=diff()
    if actual not in prefixes or diff(cached=True) not in prefixes:
        raise SystemExit('FFmpeg checkout differs from the exact recorded compatibility patches; preserve local edits')
    applied=prefixes.index(actual)
    for path in patches[applied:]:
        run(['git','-C',source,'apply','--check',path])
        run(['git','-C',source,'apply','--whitespace=nowarn',path])
    if diff()!=prefixes[-1]:
        raise SystemExit('FFmpeg compatibility patch verification failed')
    return receipts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--devkitpro', type=Path, default=Path(os.environ.get('DEVKITPRO', '/opt/devkitpro')))
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 64:
        parser.error('--jobs must be 1..64')
    dependency = json.loads((ROOT / 'dependencies-switch-video.json').read_text())['ffmpeg-nvtegra']
    source = ROOT / dependency['path']
    if not source.exists():
        run(['git', 'clone', '--depth', '1', '--branch', dependency['branch_hint'], dependency['url'], source])
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != dependency['commit']:
        if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip():
            raise SystemExit('Pinned FFmpeg checkout differs and has local edits; preserve them before changing its revision')
        run(['git', '-C', source, 'fetch', '--depth', '1', 'origin', dependency['commit']])
        run(['git', '-C', source, 'checkout', '--detach', dependency['commit']])
    patch_receipts = apply_compatibility_patches(source, dependency)
    devkit = args.devkitpro.resolve()
    build = ROOT / 'build/switch-ffmpeg'
    prefix = ROOT / 'build/switch-ffmpeg/install'
    build.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['DEVKITPRO'] = str(devkit)
    env['DEVKITA64'] = str(devkit / 'devkitA64')
    env['PATH'] = str(devkit / 'devkitA64/bin') + ':' + str(devkit / 'tools/bin') + ':' + env['PATH']
    flags = [
        '--prefix=' + str(prefix), '--target-os=horizon', '--enable-cross-compile',
        '--cross-prefix=aarch64-none-elf-', '--arch=aarch64', '--cpu=cortex-a57',
        '--enable-pic', '--enable-neon', '--enable-asm', '--enable-gpl', '--enable-nvtegra',
        '--disable-everything', '--disable-autodetect', '--disable-runtime-cpudetect',
        '--disable-doc', '--disable-programs', '--disable-debug', '--disable-network',
        '--disable-avdevice', '--disable-avfilter', '--disable-swscale', '--disable-swresample',
        '--disable-shared', '--enable-static', '--enable-avcodec', '--enable-avformat', '--enable-avutil',
        # MPEG-PS stream discovery delegates codec probing to raw MPEG video.
        # Omitting mpegvideo leaves valid PSS video as codec NONE, size 0x0.
        '--enable-decoder=mpeg2video', '--enable-parser=mpegvideo', '--enable-demuxer=mpegps,mpegvideo',
        '--enable-protocol=file', '--enable-hwaccel=mpeg2_nvtegra',
        '--extra-cflags=-std=gnu17 -D__SWITCH__ -D_GNU_SOURCE -Dtimegm=mktime -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -mno-outline-atomics -fPIE -I' + str(devkit / 'libnx/include'),
        '--extra-ldflags=-fPIE -fPIC -mtp=soft -L' + str(devkit / 'libnx/lib') + ' -specs=' + str(devkit / 'libnx/switch.specs'),
    ]
    config = {'dependency': dependency, 'patches': patch_receipts, 'flags': flags, 'compiler': subprocess.check_output([str(devkit / 'devkitA64/bin/aarch64-none-elf-gcc'), '--version'], text=True).splitlines()[0]}
    marker = build / 'night100-config.json'
    if not marker.exists() or json.loads(marker.read_text()) != config:
        run([source / 'configure', *flags], cwd=build, env=env)
        marker.write_text(json.dumps(config, indent=2) + '\n')
    run(['make', '-j' + str(args.jobs)], cwd=build, env=env)
    run(['make', 'install'], cwd=build, env=env)
    config['libraries'] = {path.name: {'size': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted((prefix / 'lib').glob('*.a'))}
    (prefix / 'night100-build.json').write_text(json.dumps(config, indent=2) + '\n')
    print('Switch NVDEC video libraries:', prefix)


if __name__ == '__main__':
    main()
