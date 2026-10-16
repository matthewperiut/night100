#!/usr/bin/env python3
"""Pack a built night100.nro into night100.patch, keyed to the game's executable.

    python3 tools/make_patch.py --elf game/SLUS_203.49 --nro build/switch/night100.nro --out night100.patch
"""
import argparse
import hashlib
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import patch_codec

ELF_SHA = 'd5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--nro', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=Path('night100.patch'))
    args = parser.parse_args()
    elf, nro = args.elf.read_bytes(), args.nro.read_bytes()
    if hashlib.sha256(elf).hexdigest() != ELF_SHA:
        sys.exit('That is not the supported USA executable (SLUS_203.49).')
    if nro[0x10:0x14] != b'NRO0':
        sys.exit('That is not a Switch NRO.')
    patch = patch_codec.seal(nro, elf, os.urandom(16), os.urandom(16))
    if patch_codec.unseal(patch, elf) != nro:
        sys.exit('Self-check failed.')
    args.out.write_bytes(patch)
    print(f'{args.out}: {len(patch) / 1e6:.1f} MB from {len(nro) / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
