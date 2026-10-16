#!/usr/bin/env python3
"""First-time setup for working on night100.

Checks the tools you need, asks for your disc image, then runs the one-time prep
(fetch PS2Recomp, apply the patches, read the disc, analyse and recompile the game).
Run it again any time; finished steps are skipped.
"""
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MIN_PYTHON = (3, 11)

INSTALL = {
    'Darwin': 'brew install cmake ninja sevenzip ffmpeg pkgconf   (and xcode-select --install for clang)',
    'Linux': 'sudo apt install git cmake ninja-build 7zip pkg-config clang libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev',
    'Windows': 'not supported yet, use WSL',
}


def have(*names):
    return next((n for n in names if shutil.which(n)), None)


def find_iso():
    folders = [HERE.parent, HERE, Path.cwd()]
    for folder in folders:
        for item in sorted(folder.glob('*.iso')):
            return item
    return None


def main():
    if sys.version_info < MIN_PYTHON:
        sys.exit(f'Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]}+ needed (this is {platform.python_version()}).')
    system = platform.system()
    print(f'night100 dev setup ({system})')

    missing = []
    for label, names in (('git', ('git',)), ('cmake', ('cmake',)), ('ninja', ('ninja',)),
                         ('7-zip (7zz)', ('7zz',)), ('pkg-config', ('pkg-config', 'pkgconf')),
                         ('a C++ compiler', ('clang++', 'c++', 'g++'))):
        found = have(*names)
        print(f'  {"ok     " if found else "MISSING"} {label}')
        if not found:
            missing.append(label)
    if shutil.which('pkg-config') and subprocess.run(['pkg-config', '--exists', 'libavcodec', 'libavformat'],
                                                     capture_output=True).returncode:
        print('  MISSING ffmpeg libraries (libavcodec, libavformat)')
        missing.append('ffmpeg libraries')
    if missing:
        print('\nInstall what is missing, then run this again:')
        print('  ' + INSTALL.get(system, 'see the README'))
        return 1

    devkitpro = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))
    if devkitpro.is_dir():
        print(f'  ok      devkitPro ({devkitpro}), Switch builds available')
    else:
        print('  skipped devkitPro, only needed for the Switch build.')
        print('          https://devkitpro.org/wiki/Getting_Started then: dkp-pacman -S switch-dev switch-zlib switch-libpng')

    iso = find_iso()
    if iso:
        print(f'\nDisc image: {iso}')
        if input('Use this one? [Y/n] ').strip().lower().startswith('n'):
            iso = None
    while not iso:
        answer = input('Path to your USA disc image (.iso) of Scooby-Doo! Night of 100 Frights: ').strip().strip('"\'')
        path = Path(answer).expanduser()
        if path.is_file():
            iso = path
        else:
            print('No such file.')

    print('\nRunning the one-time prep, this takes a while the first time.\n')
    result = subprocess.run([sys.executable, str(HERE / 'night100'), 'prepare', '--iso', str(iso.resolve())], cwd=HERE)
    if result.returncode:
        print('\nPrep failed, see logs/ for details.')
        return result.returncode

    print('''
Ready. Common commands:
  ./night100 run --skip-videos            build and run on this computer
  ./night100 test                         unit checks
  ./night100 build --platform switch      build night100.nro (needs devkitPro)
  python3 install.py                      put it on an SD card''')
    return 0


if __name__ == '__main__':
    sys.exit(main())
