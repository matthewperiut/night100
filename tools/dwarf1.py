#!/usr/bin/env python3
"""Read the retail Metrowerks ELF's DWARF 1 records without external packages."""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re
import struct


def sections(data):
    if data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('Expected a little-endian ELF32 executable')
    offset = struct.unpack_from('<I', data, 32)[0]
    size, count, names = struct.unpack_from('<HHH', data, 46)
    headers = [struct.unpack_from('<10I', data, offset + i * size) for i in range(count)]
    strings = data[headers[names][4]:headers[names][4] + headers[names][5]]
    return {strings[h[0]:].split(b'\0', 1)[0].decode(): data[h[4]:h[4]+h[5]]
            for h in headers if h[0]}


def records(data):
    offset = 0
    while offset < len(data):
        length = struct.unpack_from('<I', data, offset)[0]
        if length < 4 or offset + length > len(data):
            raise ValueError(f'Invalid DWARF record at {offset:#x}: {length}')
        if length < 6:
            offset += length
            continue
        tag = struct.unpack_from('<H', data, offset + 4)[0]
        pos, end, attrs = offset + 6, offset + length, {}
        while pos < end:
            code = struct.unpack_from('<H', data, pos)[0]
            pos += 2
            form, attribute = code & 15, code >> 4
            if form in (1, 2, 5, 6, 7):
                n = {1: 4, 2: 4, 5: 2, 6: 4, 7: 8}[form]
                value = int.from_bytes(data[pos:pos+n], 'little')
                pos += n
            elif form in (3, 4):
                n = 2 if form == 3 else 4
                size = int.from_bytes(data[pos:pos+n], 'little')
                pos += n
                value = data[pos:pos+size].hex()
                pos += size
            elif form == 8:
                stop = data.index(b'\0', pos, end)
                value = data[pos:stop].decode('utf-8', errors='replace')
                pos = stop + 1
            else:
                raise ValueError(f'Unknown form {form} at {pos-2:#x}')
            if pos > end:
                raise ValueError(f'Attribute overran record at {offset:#x}')
            attrs[attribute] = value
        yield offset, tag, attrs
        offset = end


def extract(elf, output):
    data = elf.read_bytes()
    debug = sections(data)['.debug']
    output.mkdir(parents=True, exist_ok=True)
    functions, units, counts, types = {}, [], collections.Counter(), []
    current = ''
    for offset, tag, attrs in records(debug):
        counts[tag] += 1
        if tag == 0x11:
            current = attrs.get(3, '')
            units.append({'offset': offset, 'source': current, 'low_pc': attrs.get(0x11),
                          'high_pc': attrs.get(0x12)})
        if tag in (6, 0x14) and 0x11 in attrs and 0x12 in attrs:
            start, end = attrs[0x11], attrs[0x12]
            if end > start and start >= 0x100000:
                name = attrs.get(3, f'sub_{start:08x}')
                functions[start] = {'name': name, 'start': start, 'end': end,
                                    'source': current, 'debug_offset': offset}
        if tag in (1, 2, 4, 0x13, 0x15, 0x17):
            types.append({'offset': offset, 'tag': tag, 'attributes': attrs})
    ordered = sorted(functions.values(), key=lambda f: f['start'])
    with (output / 'functions.csv').open('w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['name', 'start', 'end', 'size'])
        used = set()
        for entry in ordered:
            name = re.sub(r'[^a-zA-Z0-9_]', '_', entry['name'])
            if name in used:
                name += f"_{entry['start']:08x}"
            used.add(name)
            entry['recomp_name'] = name
            writer.writerow([name, hex(entry['start']), hex(entry['end']), entry['end']-entry['start']])
    summary = {'elf': str(elf), 'sha256': hashlib.sha256(data).hexdigest(),
               'entry': struct.unpack_from('<I', data, 24)[0], 'debug_bytes': len(debug),
               'functions': len(ordered), 'function_bytes': sum(f['end']-f['start'] for f in ordered),
               'compile_units': len(units), 'unique_sources': len({u['source'] for u in units}),
               'types': len(types), 'tag_counts': {hex(k): v for k, v in counts.items()}}
    for name, value in [('functions', ordered), ('units', units), ('types', types), ('summary', summary)]:
        (output / f'{name}.json').write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--output', type=Path, default=Path('analysis'))
    args = parser.parse_args()
    extract(args.elf, args.output)
