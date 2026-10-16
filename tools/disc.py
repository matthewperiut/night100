#!/usr/bin/env python3
"""Index physical ISO9660 extents so EE HLE and the original IOP agree on LBNs."""
import argparse
import json
from pathlib import Path
import struct


def index(iso):
    with iso.open('rb') as stream:
        stream.seek(16 * 2048)
        primary = stream.read(2048)
        if primary[:7] != b'\x01CD001\x01':
            raise ValueError('Expected an ISO9660 primary volume descriptor')
        root = primary[156:190]
        result, visited = {}, set()

        def walk(extent, size, prefix):
            if extent in visited:
                return
            visited.add(extent)
            stream.seek(extent * 2048)
            data = stream.read(size)
            pos = 0
            while pos < len(data):
                length = data[pos]
                if length == 0:
                    pos = (pos // 2048 + 1) * 2048
                    continue
                record = data[pos:pos+length]
                pos += length
                if len(record) < 34:
                    raise ValueError('Truncated directory record')
                name = record[33:33+record[32]]
                if name in (b'\x00', b'\x01'):
                    continue
                name = name.decode('ascii').split(';')[0]
                path = prefix + name
                lbn, length = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
                if record[25] & 2:
                    walk(lbn, length, path + '/')
                else:
                    if record[25] & 128:
                        raise ValueError('Multi-extent files are unsupported')
                    result[path] = {'lbn': lbn, 'size': length}

        walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')
        return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    args = parser.parse_args()
    iso = args.iso.resolve()
    entries = index(iso)
    Path('analysis').mkdir(exist_ok=True)
    Path('game').mkdir(exist_ok=True)
    Path('analysis/disc.json').write_text(json.dumps({'iso': str(iso), 'files': entries}, indent=2) + '\n')
    Path('game/.disc-map').write_text(''.join(f'{name.lower()}\t{entry["lbn"]}\t{entry["size"]}\n'
                                              for name, entry in sorted(entries.items())))
    # Confirm the extracted ELF and this disc index describe the same bytes.
    entry = entries['SLUS_203.49']
    with iso.open('rb') as stream:
        stream.seek(entry['lbn'] * 2048)
        if stream.read(entry['size']) != Path('game/SLUS_203.49').read_bytes():
            raise SystemExit('Extracted ELF does not match this disc')
    print(f'Indexed {len(entries)} disc files with physical sector addresses')
