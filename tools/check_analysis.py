#!/usr/bin/env python3
"""Cross-check the local DWARF parser against the independently generated dump."""
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parent.parent
functions = json.loads((root / 'analysis/functions.json').read_text())
reference = root / 'reference/dwarfdb/scooby/SLUS-20349'
ranges = set()
for file in reference.rglob('*'):
    if file.suffix in ('.cpp', '.c', '.h'):
        ranges.update((int(a, 16), int(b, 16)) for a, b in re.findall(
            r'Code range: (0x[0-9A-Fa-f]+) -> (0x[0-9A-Fa-f]+)', file.read_text(errors='replace')))
if not ranges:
    raise SystemExit('Missing independent dwarfdb reference checkout; run ./night100 bootstrap')
for function in functions:
    pair = function['start'], function['end']
    if pair not in ranges:
        raise SystemExit(f'Boundary mismatch: {function["name"]} {pair}')
if len(functions) != 2625:
    raise SystemExit('Unexpected function count')
print(f'PASS: all {len(functions)} local DWARF function boundaries match the independent dwarfdb dump')
