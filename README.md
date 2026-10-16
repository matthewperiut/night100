# night100

Scooby-Doo! Night of 100 Frights (PS2) as a native program, with a Nintendo Switch port.

It is a decomp of the game's code, statically recompiled with [PS2Recomp](https://github.com/ran-j/PS2Recomp)
and a new runtime and renderer. No emulator. The game ships DWARF debug info on the disc, which is
what makes the function boundaries and types reliable. You need your own copy of the game, no game
data is in this repo.

What changed from the PS2 original:

- Runs on Switch homebrew (AArch64 NRO, deko3d, hardware MPEG-2 for the movies)
- Switch code and gamepad controls with Switch button prompts, instead of the PS2 pad
- 16:9 gameplay, menus and HUD stay in their 4:3 spots
- Performance work: GPU skinning and native model draws, native particles, IOP on its own core, cheaper timers, clock boost, 720p internal resolution when docked
- macOS build for testing and development

Status: played through the first areas, 50 to 60 fps there, heavier scenes dip. Later levels are untested, expect bugs.

## Versions

Decompiled and tested with:

| | |
| --- | --- |
| Game | Scooby-Doo! Night of 100 Frights, USA (NTSC-U), SLUS-203.49 |
| Disc image | 4,480,860,160 bytes, SHA-256 `12394027092f31637a3e18c4eadf34bfa1f66077d6c075dc50b67b34ff7c4c7d` |
| Game executable | SHA-256 `d5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1` |
| PS2Recomp | `eb8688b2fd191129f103a39543002c005b33409c` plus the patches in `patches/` |
| Switch toolchain | devkitA64 r30 (GCC 16.1.0), libnx 4.12.0, deko3d 0.5.0 |
| Movie decoder | averne/FFmpeg `caeec83b791be08ed43468a2ef426d6901d51c78` (nvtegra) |
| Switch | firmware 20.0.1, Atmosphere, hbmenu 3.6.0 |

Only the exact USA executable is accepted. Other regions and revisions will not work.

## Play it on a Switch

You need a Switch running Atmosphere with hbmenu, an SD card with about 3 GB free, and Python 3.11 or newer on your computer.

1. Download `night100-switch-1.0.zip` from the [releases](../../releases) and unzip it.
2. Have an `.iso` of your own copy of the game (USA).
3. Run `python3 install.py`. It checks your setup, finds your SD card, asks for the iso and writes everything to `switch/night100` on the card.
4. Put the card in the Switch. Open hbmenu with a game launched while holding R (full memory mode, the album applet does not have enough RAM) and start Night100.

The first start verifies the game files, give it a few seconds. To update later, run `python3 install.py --update`.

## Controls

Left stick moves, right stick is the camera. B is cross (confirm, jump), A is circle (back), Y is square, X is triangle. Plus is start, minus is select. L, R, ZL, ZR as on the PS2 pad.

Debug controls, hold L and R together:

| | |
| --- | --- |
| 3 seconds | performance overlay (fps, cpu and gpu ms) |
| + X | cycle internal resolution: 1/2x, 1x, 2x (default is 1x handheld, 2x docked) |
| + Plus, 1 second | quit to hbmenu |
| + Up / Down | start / stop recording a replay (saves the state first) |
| + Left / Right | replay list, left and right to pick, B plays, A cancels |

On the desktop build: F3 overlay, F11 record, F12 stop, F10 replay list.

Launch options (nxlink or the launcher): `--aspect 4:3|16:9`, `--internal-res auto|half|1x|2x`, `--switch-clocks default`, `--no-iop-worker`.

## Build it yourself

```sh
python3 setup_dev.py                    # checks tools, asks for your iso, preps everything
./night100 run --skip-videos            # macOS build
./night100 test
./night100 build --platform switch      # night100.nro, needs devkitPro: switch-dev switch-zlib switch-libpng
python3 install.py                      # same installer as the release, picks up ../*.iso
```

`setup_dev.py` needs git, cmake, ninja, 7-zip (`7zz`), pkg-config, a C++ compiler and the ffmpeg libraries. It tells you what is missing.
Generated game code, analysis output and extracted assets stay local and are git ignored.

## Credits

[PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j, the Heavy Iron / RenderWare structure from [bfbbdecomp](https://github.com/bfbbdecomp/bfbb),
[libnx](https://github.com/switchbrew/libnx), [deko3d](https://github.com/devkitPro/deko3d), [raylib](https://www.raylib.com/),
[averne's FFmpeg nvtegra fork](https://github.com/averne/FFmpeg). GPL-3.0, see LICENSE.
