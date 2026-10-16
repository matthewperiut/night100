#!/usr/bin/env python3
"""Combine exact DWARF boundaries with PS2Recomp's SDK discovery."""
import bisect
import csv
import json
from pathlib import Path
import re
import struct
import tomllib

root = Path(__file__).resolve().parent.parent
exact = json.loads((root / 'analysis/functions.json').read_text())
# This exact retail build puts VU microprograms and data after its last EE
# function, in the same WAX section. They must not be translated as EE code.
summary = json.loads((root / 'analysis/summary.json').read_text())
if summary['sha256'] != 'd5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1':
    raise SystemExit('Unrecognized ELF build; review code/data boundaries before recompiling')
code_end = 0x2812A4
starts = [f['start'] for f in exact]
merged = {f['start']: dict(f, name=f['recomp_name']) for f in exact}
with (root / 'analysis/analyzer.toml.csv').open() as stream:
    for row in csv.DictReader(stream):
        start, end = int(row['start'], 0), int(row['end'], 0)
        if start >= code_end:
            continue
        end = min(end, code_end)
        index = bisect.bisect_right(starts, start) - 1
        if index >= 0 and start < exact[index]['end']:
            continue
        next_index = bisect.bisect_right(starts, start)
        if next_index < len(starts):
            end = min(end, starts[next_index])
        if end > start:
            merged[start] = dict(name=re.sub(r'\W', '_', row['name']), start=start, end=end)

# The recompiler also binds SDK handlers by function name. Rename helpers that
# must execute their original instructions, as well as excluding their stubs.
guest_gs_helpers = {
    'sceGszbufaddr', 'sceGsSetDefDispEnv', 'sceGsSetDefDrawEnv',
    'sceGsSetDefDrawEnv2', 'sceGsSetDefClear', 'sceGsSetDefDBuff',
    'sceGsSetDefDBuffDc', 'sceGsSetDefStoreImage',
}
for entry in merged.values():
    if entry['name'] in guest_gs_helpers:
        entry['name'] = 'guest_' + entry['name']

with (root / 'analysis/combined.csv').open('w', newline='') as stream:
    writer = csv.writer(stream)
    writer.writerow(['name', 'start', 'end', 'size'])
    for start, entry in sorted(merged.items()):
        writer.writerow([entry['name'], hex(start), hex(entry['end']), entry['end'] - start])

analyzer = tomllib.loads((root / 'analysis/analyzer.toml').read_text())
# These libgs routines build guest data packets. Preserve this disc's original
# ABI and packet layouts instead of the runtime's approximations.
stubs = [stub for stub in analyzer['general']['stubs']
         if stub.split('@')[0] not in guest_gs_helpers]
# The analyzer's raw patches are heuristic suggestions, not verified fixes.
config = (root / 'config/scooby.toml').read_text().replace('analysis/functions.csv', 'analysis/combined.csv')
config = config.replace('stubs = []', 'stubs = ' + json.dumps(stubs, indent=2))
binary = (root / 'game/SLUS_203.49').read_bytes()
# Static RenderWare driver tables reference callbacks that have no direct JAL
# callers and no DWARF. Preserve these as resumable entries without inventing
# new boundaries inside the authoritative debug functions.
data = binary[code_end - 0x100000 + 128:0x257c80]
pointers = sorted({word[0] for word in struct.iter_unpack('<I', data)
                   if 0x100008 <= word[0] < code_end and word[0] % 4 == 0})
config += '\nentry_points = ' + json.dumps([hex(p) for p in pointers], indent=2) + '\n'
config = config.replace('output = "generated"', 'output = "build/generated-stage"')
(root / 'analysis/scooby.toml').write_text(config)
print(f'Merged {len(exact)} exact functions with {len(merged)-len(exact)} discovered functions; {len(stubs)} SDK bindings.')
print(f'Recovered {len(pointers)} callback/jump targets from the original data segment.')
