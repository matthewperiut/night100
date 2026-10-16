#!/usr/bin/env python3
"""Bounded parser/decoder checks; original game files are optional local evidence."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('controller_assets', ROOT / 'tools/controller_assets.py')
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)


def hip_block(tag, body):
    return tag + struct.pack('>I', len(body)) + body


class ControllerAssets(unittest.TestCase):
    def test_hip_uses_bounded_metadata(self):
        # An AHDR-looking string in opaque DPAK payload must never become an asset.
        name = hip_block(b'ADBG', b'\0' * 4 + b'font\0\0\0\0' + b'\0' * 4)
        header = struct.pack('>I4sIIII', 0x9761067, b'RWTX', 0, 8, 0, 6) + name
        data = b'HIPA\0\0\0\0' + hip_block(b'DICT', hip_block(b'ATOC', hip_block(b'AHDR', header)))
        data += hip_block(b'DPAK', b'AHDR\xff\xff\xff\xff')
        result = assets.hip_assets(data)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]['name'], 'font')
        self.assertEqual(result[0]['id'], 0x9761067)
        with self.assertRaises(ValueError):
            assets.hip_assets(data[:-1])

    def test_transfer_permutation(self):
        self.assertEqual([assets.indexed_offset(x, 0, 16) for x in range(16)],
                         [0, 4, 8, 12, 16, 20, 24, 28, 2, 6, 10, 14, 18, 22, 26, 30])
        self.assertEqual(assets.indexed_offset(0, 2, 256), 17)
        offsets = {assets.indexed_offset(x, y, 256) for y in range(256) for x in range(256)}
        self.assertEqual(offsets, set(range(256 * 256)))

    def test_png_and_hash(self):
        self.assertEqual(assets.fnv1a64(b'hello'), 0xa430d84680aabd0b)
        rgba = bytes([255, 255, 255, 0, 8, 16, 32, 255])
        encoded = assets.png(2, 1, rgba)
        self.assertEqual(encoded[:8], b'\x89PNG\r\n\x1a\n')
        offset = 8
        image = bytearray()
        while offset < len(encoded):
            size, kind = struct.unpack_from('>I4s', encoded, offset)
            data = encoded[offset + 8:offset + 8 + size]
            crc = struct.unpack_from('>I', encoded, offset + 8 + size)[0]
            self.assertEqual(zlib.crc32(kind + data), crc)
            if kind == b'IDAT':
                image.extend(data)
            offset += size + 12
        self.assertEqual(zlib.decompress(image), b'\0' + rgba)

    @unittest.skipUnless((ROOT / 'game/FONT.HIP').exists(), 'original local disc unavailable')
    def test_original_font_and_incremental_export(self):
        archive = (ROOT / 'game/FONT.HIP').read_bytes()
        asset = next(a for a in assets.hip_assets(archive) if a['name'] == 'font')
        width, height, rgba = assets.decode_font(archive[asset['offset']:asset['offset'] + asset['size']])
        self.assertEqual((width, height), (256, 256))
        self.assertEqual(assets.sha256(rgba), assets.FONT_RGBA_SHA256)
        self.assertEqual(assets.fnv1a64(rgba), 0xbe3460f2b2f32834)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            assets.export(ROOT / 'game', output)
            times = {p.relative_to(output): p.stat().st_mtime_ns for p in output.rglob('*') if p.is_file()}
            self.assertEqual((output / 'ps/cross.png').read_bytes(), (output / 'ps/x.png').read_bytes())
            assets.export(ROOT / 'game', output)
            self.assertEqual(times, {p.relative_to(output): p.stat().st_mtime_ns for p in output.rglob('*') if p.is_file()})
            self.assertEqual(len((output / 'prompts.map').read_text().splitlines()), 11)
            self.assertEqual(struct.unpack_from('>II', (output / 'ps/menu_cross.png').read_bytes(), 16), (18, 18))
            self.assertEqual(struct.unpack_from('>II', (output / 'sw/menu_b.png').read_bytes(), 16), (18, 18))


if __name__ == '__main__':
    unittest.main()
