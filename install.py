#!/usr/bin/env python3
"""Put Night100 on a Switch SD card.

Works from the release zip or from a source checkout. You need your own disc
image of Scooby-Doo! Night of 100 Frights (USA, SLUS-203.49). Nothing from the
disc is shipped with this project; the game data is built from your image.

    python3 install.py                 interactive
    python3 install.py --iso game.iso --sd /Volumes/SWITCH_SD
    python3 install.py --update        only replace night100.nro on the card
"""
import argparse
import os
import platform
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

MIN_PYTHON = (3, 11)
HERE = Path(__file__).resolve().parent
SOURCE_TREE = (HERE / 'night100').is_file() and (HERE / 'src').is_dir()
TOOLS = HERE / 'tools'
NEED_SD_GB = 2.4
NEED_WORK_GB = 2.4


def say(text=''):
    print(text, flush=True)


def ask(prompt, default=None):
    suffix = f' [{default}]' if default else ''
    try:
        value = input(f'{prompt}{suffix}: ').strip()
    except EOFError:
        value = ''
    return value or (default or '')


def confirm(prompt, default=True, assume_yes=False):
    if assume_yes:
        return True
    value = ask(f'{prompt} ({"Y/n" if default else "y/N"})').lower()
    return default if not value else value.startswith('y')


def gb(value):
    return value / 1e9


# ---------------------------------------------------------------- environment

def ensure_python():
    """Needs Python 3.11+. Re-runs itself with a newer interpreter if one is installed."""
    if sys.version_info >= MIN_PYTHON:
        return
    names = ['python3.14', 'python3.13', 'python3.12', 'python3.11']
    for name in names:
        found = shutil.which(name)
        if found:
            say(f'Python {sys.version_info.major}.{sys.version_info.minor} is too old, switching to {found}')
            os.execv(found, [found] + sys.argv)
    if platform.system() == 'Windows' and shutil.which('py'):
        for version in ('3.14', '3.13', '3.12', '3.11'):
            probe = subprocess.run(['py', f'-{version}', '-c', 'pass'], capture_output=True)
            if probe.returncode == 0:
                raise SystemExit(subprocess.call(['py', f'-{version}'] + sys.argv))
    system = platform.system()
    hint = {'Darwin': 'brew install python   (or https://www.python.org/downloads/)',
            'Windows': 'install Python 3.11 or newer from https://www.python.org/downloads/',
            }.get(system, 'sudo apt install python3   (or your distro equivalent, Python 3.11 or newer)')
    raise SystemExit(f'This needs Python {MIN_PYTHON[0]}.{MIN_PYTHON[1]} or newer, found '
                     f'{sys.version_info.major}.{sys.version_info.minor}.\nInstall it with: {hint}')


def check_files():
    missing = []
    for name in ('package_resources.py', 'package_switch.py'):
        if not (TOOLS / name).is_file():
            missing.append(f'tools/{name}')
    for name in ('dependencies.json', 'dependencies-switch-video.json'):
        if not (HERE / name).is_file():
            missing.append(name)
    if missing:
        raise SystemExit('Missing files next to install.py: ' + ', '.join(missing) +
                         '\nExtract the whole release zip, do not copy install.py on its own.')


def find_nro(explicit):
    candidates = [Path(explicit)] if explicit else [HERE / 'night100.nro', HERE / 'build/switch/night100.nro']
    for path in candidates:
        if path.is_file():
            with path.open('rb') as stream:
                stream.seek(0x10)
                if stream.read(4) == b'NRO0':
                    return path
            raise SystemExit(f'{path} is not a Switch NRO')
    if SOURCE_TREE:
        raise SystemExit('No night100.nro yet. Build it first: ./night100 build --platform switch '
                         '(needs devkitPro, see the README), or pass --nro PATH.')
    raise SystemExit('night100.nro is missing from the release folder.')


# ------------------------------------------------------------------- the disc

def iso_candidates(extra):
    folders = []
    for folder in extra + ([HERE.parent] if SOURCE_TREE else []) + [HERE, Path.cwd(), Path.home() / 'Downloads',
                                                                    Path.home() / 'Desktop', Path.home() / 'Documents']:
        if folder and folder.is_dir() and folder not in folders:
            folders.append(folder)
    found = []
    for folder in folders:
        try:
            for item in sorted(folder.iterdir()):
                if item.suffix.lower() == '.iso' and item.is_file() and item not in found:
                    found.append(item)
        except OSError:
            pass
    def score(path):
        name = path.name.lower()
        return (('scooby' in name) + ('100' in name) + ('frights' in name), path.stat().st_size > 3_000_000_000)
    return sorted(found, key=score, reverse=True)


def looks_like_game(iso):
    """Quick structural check that the image is a readable ISO9660 disc holding the game executable."""
    try:
        sys.path.insert(0, str(TOOLS))
        import package_resources
        index = package_resources.index_iso(iso)
        return any(str(name).upper().replace('\\', '/').split('/')[-1].startswith('SLUS_203.49') for name in index)
    except Exception as error:  # structural problems are reported to the user
        return f'{error}'


def pick_iso(args):
    if args.resources:
        return None
    if args.iso:
        path = Path(args.iso).expanduser()
        if not path.is_file():
            raise SystemExit(f'Disc image not found: {path}')
        return path
    found = iso_candidates([])
    if SOURCE_TREE and found and found[0].parent == HERE.parent:
        say(f'Using the disc image next to the source folder: {found[0]}')
        return found[0]
    say()
    say('Disc image (.iso) of Scooby-Doo! Night of 100 Frights, USA release (SLUS-203.49)')
    if found:
        for number, item in enumerate(found[:8], 1):
            say(f'  {number}) {item}  ({gb(item.stat().st_size):.2f} GB)')
        say('  or type a path to another file')
    while True:
        answer = ask('Select' if found else 'Path to the .iso')
        if answer.isdigit() and 1 <= int(answer) <= min(len(found), 8):
            return found[int(answer) - 1]
        path = Path(answer.strip('"\'')).expanduser()
        if path.is_file():
            return path
        say('That file does not exist.')


# ---------------------------------------------------------------- the SD card

class Volume:
    def __init__(self, path, label='', fs='', free=0, removable=False):
        self.path, self.label, self.fs, self.free, self.removable = Path(path), label, fs, free, removable
        self.switch = (self.path / 'atmosphere').is_dir() or (self.path / 'switch').is_dir()
        self.cfw = (self.path / 'atmosphere').is_dir()

    def rank(self):
        return (self.cfw * 4) + (self.switch * 2) + self.removable

    def describe(self):
        bits = [self.label or self.path.name or str(self.path), self.fs, f'{gb(self.free):.1f} GB free']
        if self.cfw:
            bits.append('Atmosphere found')
        elif self.switch:
            bits.append('has a switch folder')
        return f'{self.path}   ({", ".join(b for b in bits if b)})'


FAT_TYPES = {'msdos', 'exfat', 'vfat', 'fat32', 'fat', 'fuseblk'}


def detect_volumes():
    system = platform.system()
    found = []
    try:
        if system == 'Darwin':
            for entry in sorted(Path('/Volumes').iterdir()):
                if entry.is_symlink() or not entry.is_dir() or not os.path.ismount(entry):
                    continue
                info = {}
                try:
                    out = subprocess.run(['diskutil', 'info', '-plist', str(entry)], capture_output=True, timeout=10)
                    info = plistlib.loads(out.stdout) if out.returncode == 0 else {}
                except (OSError, subprocess.TimeoutExpired, plistlib.InvalidFileException):
                    pass
                fs = str(info.get('FilesystemType', '')).lower()
                if fs and fs not in FAT_TYPES:
                    continue
                found.append(Volume(entry, info.get('VolumeName', entry.name), fs,
                                    shutil.disk_usage(entry).free, bool(info.get('RemovableMedia') or info.get('Ejectable'))))
        elif system == 'Windows':
            import ctypes
            kernel = ctypes.windll.kernel32
            mask = kernel.GetLogicalDrives()
            for index in range(26):
                if not mask & (1 << index):
                    continue
                root = f'{chr(65 + index)}:\\'
                kind = kernel.GetDriveTypeW(root)
                label = ctypes.create_unicode_buffer(261)
                fs = ctypes.create_unicode_buffer(261)
                kernel.GetVolumeInformationW(root, label, 261, None, None, None, fs, 261)
                volume = Volume(root, label.value, fs.value.lower(), shutil.disk_usage(root).free, kind == 2)
                if kind == 2 or volume.switch:
                    found.append(volume)
        else:
            with open('/proc/mounts') as mounts:
                for line in mounts:
                    device, mountpoint, fs = line.split()[:3]
                    mountpoint = mountpoint.replace('\\040', ' ')
                    if fs not in FAT_TYPES or mountpoint.startswith(('/boot', '/efi')):
                        continue
                    if not mountpoint.startswith(('/media', '/run/media', '/mnt')) and not device.startswith(('/dev/mmcblk', '/dev/sd')):
                        continue
                    found.append(Volume(mountpoint, Path(mountpoint).name, fs, shutil.disk_usage(mountpoint).free,
                                        device.startswith('/dev/mmcblk') or mountpoint.startswith(('/media', '/run/media'))))
    except OSError:
        pass
    return sorted(found, key=lambda v: v.rank(), reverse=True)


def normalise_sd_root(path):
    path = Path(path).expanduser()
    if path.name == 'night100' and path.parent.name == 'switch':
        return path.parent.parent
    if path.name == 'switch':
        return path.parent
    return path


def pick_sd(args):
    if args.sd:
        root = normalise_sd_root(args.sd)
        if not root.is_dir():
            raise SystemExit(f'SD card folder not found: {root}')
        return root
    volumes = detect_volumes()
    say()
    say('SD card (the one you use in the Switch)')
    for number, volume in enumerate(volumes, 1):
        say(f'  {number}) {volume.describe()}' + ('   <- suggested' if number == 1 and volume.switch else ''))
    if not volumes:
        say('  no FAT32/exFAT cards detected. Insert the card, or type its path.')
    else:
        say('  or type a path')
    while True:
        answer = ask('Select' if volumes else 'Path to the SD card')
        if answer.isdigit() and 1 <= int(answer) <= len(volumes):
            return volumes[int(answer) - 1].path
        if answer:
            path = normalise_sd_root(answer.strip('"\''))
            if path.is_dir():
                return path
            say('That folder does not exist.')


# -------------------------------------------------------------------- install

def run(command):
    say('+ ' + ' '.join(str(part) for part in command))
    result = subprocess.run([str(part) for part in command])
    if result.returncode:
        raise SystemExit(f'Step failed (exit {result.returncode}).')


def copy_nro(nro, target):
    target.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=target, prefix='.night100-nro-', delete=False) as stream:
        temporary = Path(stream.name)
        with nro.open('rb') as source:
            shutil.copyfileobj(source, stream, 1024 * 1024)
    os.replace(temporary, target / 'night100.nro')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--iso', help='disc image of the USA release')
    parser.add_argument('--sd', help='SD card root (or its switch folder)')
    parser.add_argument('--nro', help='night100.nro to install (default: the one next to this script)')
    parser.add_argument('--resources', help='already packaged resources (resource-manifest.json inside), skips the disc image')
    parser.add_argument('--workdir', help='scratch folder for the packaged game data (needs about 2.4 GB)')
    parser.add_argument('--update', action='store_true', help='only replace night100.nro on a card that is already set up')
    parser.add_argument('--check', action='store_true', help='check this machine and stop')
    parser.add_argument('--keep-work', action='store_true', help='keep the scratch folder after installing')
    parser.add_argument('-y', '--yes', action='store_true', help='do not ask for confirmation')
    args = parser.parse_args()

    ensure_python()
    check_files()
    nro = find_nro(args.nro)
    say(f'Night100 installer  (Python {sys.version_info.major}.{sys.version_info.minor}, '
        f'{"source tree" if SOURCE_TREE else "release"} mode)')
    say(f'Executable: {nro.name}, {nro.stat().st_size / 1e6:.1f} MB')
    if args.check:
        say('This machine is ready.')
        return 0

    sd_root = pick_sd(args)
    target = sd_root / 'switch' / 'night100'
    if not (sd_root / 'atmosphere').is_dir() and not (sd_root / 'switch').is_dir():
        say(f'\n{sd_root} has no atmosphere or switch folder, so it may not be your Switch card.')
        if not confirm('Install here anyway?', False, args.yes):
            return 1

    if args.update:
        if not (target / 'asset-manifest.json').is_file():
            raise SystemExit(f'{target} has no game data yet. Run without --update first.')
        copy_nro(nro, target)
        say(f'\nUpdated {target / "night100.nro"}')
        return 0

    already = (target / 'asset-manifest.json').is_file()
    if already:
        say(f'\n{target} already holds the game data.')
        if confirm('Only replace night100.nro?', True, args.yes):
            copy_nro(nro, target)
            say('Updated night100.nro')
            return 0

    resources = Path(args.resources).expanduser() if args.resources else None
    if not resources and SOURCE_TREE:
        for candidate in (HERE.parent / 'out/switch-resources', HERE / 'out/switch-resources'):
            if (candidate / 'resource-manifest.json').is_file():
                resources = candidate
                say(f'Using packaged resources in {candidate}')
                break
    iso = None if resources else pick_iso(args)
    if iso:
        verdict = looks_like_game(iso)
        if verdict is not True:
            raise SystemExit(f'{iso.name} does not look like the USA disc image: '
                             f'{"SLUS_203.49 not found on the disc" if verdict is False else verdict}\n'
                             'This needs the .iso of Scooby-Doo! Night of 100 Frights (USA). Other regions and CUE/BIN images are not supported.')

    work = Path(args.workdir).expanduser() if args.workdir else Path(tempfile.gettempdir()) / 'night100-install'
    free_sd = shutil.disk_usage(sd_root).free
    if not already and free_sd < NEED_SD_GB * 1e9:
        raise SystemExit(f'The SD card has {gb(free_sd):.1f} GB free, about {NEED_SD_GB} GB are needed.')
    if iso:
        work.mkdir(parents=True, exist_ok=True)
        free_work = shutil.disk_usage(work).free
        if free_work < NEED_WORK_GB * 1e9:
            raise SystemExit(f'{work} has {gb(free_work):.1f} GB free, about {NEED_WORK_GB} GB are needed for scratch space.\n'
                             'Pass --workdir with a folder on a bigger drive.')

    say()
    say(f'Disc image : {iso if iso else resources}')
    say(f'SD card    : {sd_root}')
    say(f'Writing to : {target}')
    if not confirm('Go ahead?', True, args.yes):
        return 1

    python = sys.executable
    if iso:
        packaged = work / 'resources'
        if packaged.exists():
            shutil.rmtree(packaged)
        say('\nStep 1/2: reading the disc image (a few minutes)')
        run([python, TOOLS / 'package_resources.py', '--iso', iso, '--output', packaged])
        resources = packaged
    say('\nStep 2/2: writing the game data and night100.nro to the SD card (a few minutes)')
    run([python, TOOLS / 'package_switch.py', '--binary', nro, '--resources', resources, '--output', target])

    if iso and not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)

    say()
    say('Done. Next steps on the Switch:')
    say('  1. Put the card back in and boot Atmosphere.')
    say('  2. Open hbmenu in full-memory mode: start a game, hold R while it launches, then pick Night100.')
    say('     (The album / applet mode does not have enough memory.)')
    say('  3. Pick Night100 in the list. The first start checks the game files and takes a few seconds.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit('\nStopped.')
