#!/usr/bin/env python3
"""Export original face-button cells from the verified USA disc's font/menu atlases.

No third-party Python packages are required. Original assets are written only to
the ignored controllers/ps directory. Switch artwork is authored independently
in controllers/sw; --render-switch rasterizes those SVGs with macOS AppKit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib


ROOT = Path(__file__).resolve().parents[1]
ELF_SHA256 = 'd5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1'
FONT_HIP_SHA256 = 'f7cae38f70b75753ff17f4da291eae20b9f1aeb93ca97dde95098a9cc3716834'
FONT_RGBA_SHA256 = '8aa96a4e294e18af0883ea17ea226897dddc20353480e316ed77c8c1325a3fbc'
CELLS = [('circle', 'a', 0, '~'), ('square', 'y', 18, '{'),
         ('triangle', 'x', 36, '}'), ('cross', 'b', 54, '#')]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_changed(path, data):
    """Keep timestamps stable for the normal build's incremental asset step."""
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def fnv1a64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def blocks(data, start, end, endian):
    """Yield bounded HIP (8-byte) or RenderWare (12-byte) blocks."""
    header = 8 if endian == '>' else 12
    while start < end:
        if end - start < header:
            raise ValueError('Truncated block header')
        kind, size = struct.unpack_from(endian + 'II', data, start)
        body = start + header
        stop = body + size
        if stop > end:
            raise ValueError('Block exceeds parent bounds')
        yield kind, body, stop
        start = stop


def hip_assets(data):
    """Read actual nested HIP metadata, never search opaque asset payloads."""
    if data[:8] != b'HIPA\0\0\0\0':
        raise ValueError('Expected HIPA archive')
    assets = []

    def visit(start, end):
        for kind, body, stop in blocks(data, start, end, '>'):
            tag = kind.to_bytes(4, 'big')
            if tag in (b'DICT', b'ATOC'):
                visit(body, stop)
            elif tag == b'AHDR':
                if stop - body < 24:
                    raise ValueError('Truncated asset header')
                ident, asset_type, offset, size, _, _ = struct.unpack_from('>I4sIIII', data, body)
                if offset > len(data) or size > len(data) - offset:
                    raise ValueError('Asset exceeds archive bounds')
                name = None
                for child, begin, finish in blocks(data, body + 24, stop, '>'):
                    if child == int.from_bytes(b'ADBG', 'big'):
                        if finish - begin < 8:
                            raise ValueError('Truncated asset debug name')
                        name = data[begin + 4:finish - 4].split(b'\0', 1)[0].decode('ascii')
                assets.append({'id': ident, 'type': asset_type.decode('ascii'),
                               'offset': offset, 'size': size, 'name': name})

    # HIPA is a zero-length sentinel; the real top-level blocks follow it.
    visit(8, len(data))
    return assets


def indexed_offset(x, y, width):
    """Match native_scene.cpp's PS2 indexed transfer-order permutation."""
    x ^= (((y >> 1) ^ (y >> 2)) & 1) << 2
    nx, ny = (x & 7) | ((x >> 1) & ~7), (y & 1) | ((y >> 1) & ~1)
    offset = ((y >> 1) & 1) | (((x >> 3) & 1) << 1) | (nx << 2) | (ny << width.bit_length())
    return (y & ~3) * width + (offset & ((width << 2) - 1))


def decode_font(data):
    """Decode a bounded PAL8, single-level, new-style PS2 RenderWare atlas."""
    outer = list(blocks(data, 0, len(data), '<'))
    if len(outer) != 1 or outer[0][0] != 0x16:
        raise ValueError('Expected one RenderWare texture dictionary')
    dictionary = list(blocks(data, outer[0][1], outer[0][2], '<'))
    native = [item for item in dictionary if item[0] == 0x15]
    if len(native) != 1:
        raise ValueError('Expected one native font texture')
    chunks = list(blocks(data, native[0][1], native[0][2], '<'))
    if data[chunks[0][1]:chunks[0][1] + 4] != b'PS2\0':
        raise ValueError('Expected PS2 native texture')
    rasters = [item for item in chunks if item[0] == 1]
    if len(rasters) != 2:
        raise ValueError('Expected native texture and raster structures')
    raster = list(blocks(data, rasters[1][1], rasters[1][2], '<'))
    if len(raster) != 2 or any(item[0] != 1 for item in raster) or raster[0][2] - raster[0][1] != 64:
        raise ValueError('Unsupported native raster structure')
    header = raster[0][1]
    width, height, depth, fmt = struct.unpack_from('<IIII', data, header)
    pixel_size, palette_size = struct.unpack_from('<II', data, header + 48)
    version = fmt >> 16
    if (not width or not height or width > 2048 or height > 2048 or width & (width - 1)
            or height % 4 or depth != 8 or version != 2
            or pixel_size != width * height + 0x50 or palette_size != 0x450):
        raise ValueError('Unsupported single-level PAL8 raster format')
    body, end = raster[1][1], raster[1][2]
    if end - body != pixel_size + palette_size:
        raise ValueError('Unexpected font texture payload size')
    # Each transfer has a 0x50-byte GIF header before its pixels/palette.
    pixels, palette = body + 0x50, body + pixel_size + 0x50
    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            index = data[pixels + indexed_offset(x, y, width)]
            index = (index & ~0x18) | ((index & 8) << 1) | ((index & 16) >> 1)
            color = data[palette + index * 4:palette + index * 4 + 4]
            dest = (y * width + x) * 4
            rgba[dest:dest + 4] = color[:3] + bytes([min(255, color[3] * 2)])
    return width, height, bytes(rgba)


def png(width, height, rgba):
    if len(rgba) != width * height * 4:
        raise ValueError('Wrong RGBA image size')

    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))

    rows = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def export(game, output):
    if sha256((game / 'SLUS_203.49').read_bytes()) != ELF_SHA256:
        raise ValueError('Controller assets require the exact supported USA ELF')
    archive = (game / 'FONT.HIP').read_bytes()
    if sha256(archive) != FONT_HIP_SHA256:
        raise ValueError('FONT.HIP does not match the supported disc')
    font = [a for a in hip_assets(archive) if a['id'] == 0x09761067 and a['type'] == 'RWTX' and a['name'] == 'font']
    if len(font) != 1:
        raise ValueError('Expected the original font asset')
    asset = font[0]
    raw = archive[asset['offset']:asset['offset'] + asset['size']]
    width, height, rgba = decode_font(raw)
    if sha256(rgba) != FONT_RGBA_SHA256:
        raise ValueError('Decoded font identity mismatch')
    ps = output / 'ps'
    ps.mkdir(parents=True, exist_ok=True)
    manifest = {'schema': 1, 'elf_sha256': ELF_SHA256, 'archive': 'FONT.HIP',
                'archive_sha256': sha256(archive), 'asset': {**asset, 'sha256': sha256(raw)},
                'atlas': {'width': width, 'height': height, 'rgba_sha256': sha256(rgba),
                          'rgba_fnv1a64': f'{fnv1a64(rgba):016x}'}, 'prompts': []}
    mapping = ['NIGHT100_PROMPTS 1']
    for name, switch, x, character in CELLS:
        rect = [x, 132, 18, 22]
        cell = b''.join(rgba[((y * width + x) * 4):((y * width + x + 18) * 4)] for y in range(132, 154))
        encoded = png(18, 22, cell)
        write_changed(ps / f'{name}.png', encoded)
        if name == 'cross':
            write_changed(ps / 'x.png', encoded)
        manifest['prompts'].append({'ps': name, 'sw': switch.upper(), 'font_character': character,
                                    'rect': rect, 'uv': [x / width, 132 / height, (x + 18) / width, 154 / height],
                                    'png_sha256': sha256(encoded)})
        mapping.append(f'{fnv1a64(rgba):016x} {width} {height} ' + ' '.join(map(str, rect)) + f' {name}.png {switch}.png')
    # These glyphs are baked into menu model textures, independently of xFont.
    # Rectangles stop before the adjacent Exit/Choose lettering. Their opaque
    # backing colors are repeated in the independently authored replacement SVGs.
    menu_sources = [
        ('text_labels2.RW3', 0xadf716bd, 0x089d88bddeb8e1d6,
         [('menu_triangle', 'menu_x', [0, 0, 18, 17]),
          ('menu_cross', 'menu_b', [0, 17, 18, 18])]),
        ('PS2_2.RW3', 0xa04aa936, 0x810e6dcda1e47e01,
         [('diagram_triangle', 'diagram_x', [64, 0, 16, 17]),
          ('diagram_square', 'diagram_y', [80, 0, 16, 17]),
          ('diagram_circle', 'diagram_a', [96, 0, 16, 17]),
          ('diagram_cross', 'diagram_b', [112, 0, 16, 17])]),
    ]
    menu_data = (game / 'MN/MNU3.HIP').read_bytes()
    menu_assets = hip_assets(menu_data)
    manifest['additional_atlases'] = []
    for source_name, ident, expected_hash, replacements in menu_sources:
        entry = next((a for a in menu_assets if a['name'] == source_name and a['id'] == ident and a['type'] == 'RWTX'), None)
        if entry is None:
            raise ValueError(f'Missing original menu texture {source_name}')
        raw = menu_data[entry['offset']:entry['offset'] + entry['size']]
        aw, ah, atlas = decode_font(raw)
        if (aw, ah, fnv1a64(atlas)) != (128, 128, expected_hash):
            raise ValueError(f'Decoded menu texture identity mismatch: {source_name}')
        record = {'archive': 'MN/MNU3.HIP', 'also_present_in': 'MN/MNU5.HIP',
                  'asset': {**entry, 'sha256': sha256(raw)}, 'width': aw, 'height': ah,
                  'rgba_sha256': sha256(atlas), 'rgba_fnv1a64': f'{expected_hash:016x}', 'prompts': []}
        for name, switch, rect in replacements:
            x, y, rw, rh = rect
            cell = b''.join(atlas[(row * aw + x) * 4:(row * aw + x + rw) * 4] for row in range(y, y + rh))
            encoded = png(rw, rh, cell)
            write_changed(ps / f'{name}.png', encoded)
            record['prompts'].append({'ps': name, 'sw': switch, 'rect': rect, 'png_sha256': sha256(encoded)})
            mapping.append(f'{expected_hash:016x} {aw} {ah} ' + ' '.join(map(str, rect)) + f' {name}.png {switch}.png')
        manifest['additional_atlases'].append(record)
    write_changed(ps / 'manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())
    write_changed(output / 'prompts.map', ('\n'.join(mapping) + '\n').encode())
    switch_dir = output / 'sw'
    switch_dir.mkdir(parents=True, exist_ok=True)
    for source in sorted((ROOT / 'controllers/sw').iterdir()):
        if source.suffix not in ('.svg', '.png'):
            continue
        destination = switch_dir / source.name
        if destination.resolve() != source.resolve():
            write_changed(destination, source.read_bytes())
    print(f'Exported ten face-button cells from the original font and menu atlases to {ps}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=ROOT / 'game')
    parser.add_argument('--output', type=Path, default=ROOT / 'controllers')
    parser.add_argument('--render-switch', action='store_true', help='also render authored SVGs with macOS AppKit')
    args = parser.parse_args()
    export(args.game, args.output)
    if args.render_switch:
        subprocess.run(['swift', str(ROOT / 'tools/rasterize_controller_svg.swift'), str(args.output / 'sw')], check=True)


if __name__ == '__main__':
    main()
