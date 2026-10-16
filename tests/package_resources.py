"""Synthetic ISO evidence for lossless, safe, reproducible resource packages."""
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_resources", ROOT / "tools/package_resources.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)
SECTOR = 2048
ELF = b"\x7fELFsynthetic-unit-test-executable\x00"
ELF_HASH = hashlib.sha256(ELF).hexdigest()


def both(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def record(name, lbn, length, directory=False):
    name = name if isinstance(name, bytes) else (name + ("" if directory else ";1")).encode("ascii")
    result = bytearray(33 + len(name) + (not len(name) % 2))
    result[0] = len(result)
    result[2:10] = both(lbn)
    result[10:18] = both(length)
    result[25] = 2 if directory else 0
    result[28:32] = struct.pack("<H", 1) + struct.pack(">H", 1)
    result[32] = len(name)
    result[33:33 + len(name)] = name
    return result


def synthetic_iso(path, extra=None):
    trailer = b"movie-trailer\x80\x00" * 257
    files = {"SLUS_203.49": ELF, "SYSTEM.CNF": b"BOOT2=cdrom0:\\SLUS_203.49;1\r\n",
             "MODULES/PADMAN.IRX": b"pad-driver", "IOPRP243.IMG": b"identical-iop-image",
             "MODULES/IOPRP243.IMG": b"identical-iop-image", "FMV/INTRO.PSS": b"intro-movie",
             "FMV/SDTRAILR.PSS": trailer, "FMV/DEMO/DEMO1.PSS": b"same-demo-stream",
             "FMV/DEMO/DEMO2.PSS": b"same-demo-stream", "PAD.PAD": trailer, "EMPTY.DAT": b""}
    files.update(extra or {})
    directories = {""}
    for name in files:
        parent = Path(name).parent
        while parent.as_posix() != ".":
            directories.add(parent.as_posix())
            parent = parent.parent
    dir_lbn = {name: 20 + i for i, name in enumerate(sorted(directories))}
    file_lbn, next_sector = {}, 600
    for name, data in files.items():
        file_lbn[name] = next_sector
        next_sector += max(1, (len(data) + SECTOR - 1) // SECTOR)
    image = bytearray(4 * 1024 * 1024)
    image[16 * SECTOR:16 * SECTOR + 7] = b"\x01CD001\x01"
    image[16 * SECTOR + 80:16 * SECTOR + 88] = both(len(image) // SECTOR)
    image[16 * SECTOR + 128:16 * SECTOR + 132] = struct.pack("<H", SECTOR) + struct.pack(">H", SECTOR)
    root = record(b"\x00", dir_lbn[""], SECTOR, True)
    image[16 * SECTOR + 156:16 * SECTOR + 156 + len(root)] = root
    positions = {}
    for directory in sorted(directories):
        pos = dir_lbn[directory] * SECTOR
        parent = Path(directory).parent.as_posix()
        parent = "" if parent == "." else parent
        for data in (record(b"\x00", dir_lbn[directory], SECTOR, True), record(b"\x01", dir_lbn.get(parent, dir_lbn[""]), SECTOR, True)):
            image[pos:pos + len(data)] = data
            pos += len(data)
        children = [(name, True) for name in directories if name and (Path(name).parent.as_posix() if Path(name).parent.as_posix() != "." else "") == directory]
        children += [(name, False) for name in files if (Path(name).parent.as_posix() if Path(name).parent.as_posix() != "." else "") == directory]
        for name, is_directory in sorted(children):
            data = record(Path(name).name, dir_lbn[name] if is_directory else file_lbn[name], SECTOR if is_directory else len(files[name]), is_directory)
            positions[name] = pos
            image[pos:pos + len(data)] = data
            pos += len(data)
    for name, data in files.items():
        offset = file_lbn[name] * SECTOR
        image[offset:offset + len(data)] = data
    # Include nonzero unlisted metadata and incompressible PAD/tail data. Neither
    # can be replaced with zeros merely because it is outside listed files.
    image[100 * SECTOR:100 * SECTOR + 333] = b"metadata-gap!" * 25 + b"\x01" * 8
    image[2 * 1024 * 1024:3 * 1024 * 1024] = random.Random(41).randbytes(1024 * 1024)
    path.write_bytes(image)
    return bytes(image), files, positions


class ResourcePackaging(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.iso = self.base / "game.iso"
        self.source, self.files, self.positions = synthetic_iso(self.iso)
        self.output = self.base / "package"

    def package(self):
        return PACKAGE.package(self.iso, self.output, expected_elf_sha256=ELF_HASH)

    def verify(self):
        return PACKAGE.verify(self.output, expected_elf_sha256=ELF_HASH)

    def patch(self, offset, data):
        with self.iso.open("r+b") as stream:
            stream.seek(offset)
            stream.write(data)

    def test_lossless_reconstruction_and_all_assets_with_single_payload(self):
        manifest = self.package()
        self.assertEqual(b"".join(PACKAGE.logical_blocks(self.output / "game")), self.source)
        result = self.verify()
        self.assertTrue(result["verified"])
        self.assertEqual(result["logical_sha256"], hashlib.sha256(self.source).hexdigest())
        self.assertEqual(result["asset_count"], len(self.files) - 1)
        for name, content in self.files.items():
            if name == "PAD.PAD":
                self.assertFalse((self.output / "game" / name).exists())
            else:
                self.assertEqual((self.output / "game" / name).read_bytes(), content)
        self.assertTrue((self.output / "game/MODULES/IOPRP243.IMG").is_symlink())
        self.assertEqual((self.output / "game/MODULES/IOPRP243.IMG").readlink().as_posix(), "../IOPRP243.IMG")
        self.assertTrue((self.output / "game/FMV/DEMO/DEMO2.PSS").is_symlink())
        self.assertIn("pad.pad\t", (self.output / "game/.disc-map").read_text())
        self.assertTrue(any(entry["path"] == "PAD.PAD" and entry["canonical"] == "FMV/SDTRAILR.PSS" for entry in manifest["deduplicated"]))
        self.assertLess(manifest["resource_bytes"], len(self.source) / 2)
        _, extents = PACKAGE.read_descriptor(self.output / "game")
        self.assertEqual({entry[4] for entry in extents}, {PACKAGE.RAW, PACKAGE.ZERO, PACKAGE.ZLIB})
        self.assertTrue(all(entry[5] != "PAD.PAD" for entry in extents))

    def test_reproducible_manifests_and_descriptor(self):
        first = self.package()
        other = self.base / "other"
        second = PACKAGE.package(self.iso, other, expected_elf_sha256=ELF_HASH)
        self.assertEqual(first, second)
        self.assertEqual((self.output / PACKAGE.MANIFEST).read_bytes(), (other / PACKAGE.MANIFEST).read_bytes())
        self.assertEqual((self.output / "game/.disc-image").read_bytes(), (other / "game/.disc-image").read_bytes())

    def test_existing_directory_or_symlink_never_replaced(self):
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_text("original")
        with self.assertRaises(FileExistsError):
            self.package()
        self.assertEqual(marker.read_text(), "original")
        self.output.rmdir() if not marker.exists() else marker.unlink()
        self.output.rmdir()
        self.output.symlink_to(self.base / "missing")
        with self.assertRaises(FileExistsError):
            self.package()
        self.assertTrue(self.output.is_symlink())

    def test_atomic_publisher_refuses_existing_empty_directory(self):
        source, target = self.base / "stage", self.base / "final"
        source.mkdir()
        (source / "data").write_text("new")
        target.mkdir()
        with self.assertRaises(FileExistsError):
            PACKAGE.publish(source, target)
        self.assertTrue((source / "data").exists())
        self.assertEqual(list(target.iterdir()), [])

    def test_wrong_elf_rejected_before_output(self):
        with self.assertRaisesRegex(ValueError, "ELF SHA256 mismatch"):
            PACKAGE.package(self.iso, self.output)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.base.glob(".package.resources-*")), [])

    def test_case_duplicate_and_reserved_collisions_rejected(self):
        for extra in ({"DUP.DAT": b"a", "dup.dat": b"b"}, {".disc-image": b"bad"}, {".disc-data/EVIL": b"bad"}):
            with self.subTest(extra=extra):
                synthetic_iso(self.iso, extra)
                with self.assertRaisesRegex(ValueError, "collision"):
                    self.package()
                self.assertFalse(self.output.exists())

    def test_path_traversal_backslash_and_control_names_rejected(self):
        for bad in (b"../EVIL;1", b"\\EVIL;1", b"/EVIL;1", b"X\tY;1"):
            with self.subTest(bad=bad):
                synthetic_iso(self.iso)
                pos = self.positions["SYSTEM.CNF"]
                self.patch(pos + 32, bytes((len(bad),)) + bad)
                with self.assertRaises(ValueError):
                    self.package()

    def test_overlapping_and_out_of_bounds_extents_rejected(self):
        pos = self.positions["SYSTEM.CNF"]
        self.patch(pos + 2, both(600))  # collides with ELF
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            self.package()
        synthetic_iso(self.iso)
        self.patch(pos + 2, both(999999))
        with self.assertRaisesRegex(ValueError, "out-of-range"):
            self.package()

    def test_multi_extent_interleaved_and_endian_mismatch_rejected(self):
        pos = self.positions["SYSTEM.CNF"]
        for offset, value in ((25, b"\x80"), (26, b"\x01"), (6, b"\xff\xff\xff\xff")):
            with self.subTest(offset=offset):
                synthetic_iso(self.iso)
                self.patch(pos + offset, value)
                with self.assertRaises(ValueError):
                    self.package()

    def test_unique_nonzero_pad_is_compressed_losslessly(self):
        self.source, _, _ = synthetic_iso(self.iso, {"PAD.PAD": b"unique-pad-not-a-file-copy" * 23})
        manifest = self.package()
        self.assertFalse(any(entry["path"] == "PAD.PAD" for entry in manifest["deduplicated"]))
        self.assertEqual(b"".join(PACKAGE.logical_blocks(self.output / "game")), self.source)

    def test_corrupt_canonical_file_and_alias_rejected(self):
        self.package()
        canonical = self.output / "game/IOPRP243.IMG"
        original = canonical.read_bytes()
        canonical.write_bytes(b"bad")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.verify()
        canonical.write_bytes(original)
        alias = self.output / "game/MODULES/IOPRP243.IMG"
        alias.unlink()
        alias.symlink_to("../../outside")
        with self.assertRaisesRegex(ValueError, "alias mismatch"):
            self.verify()

    def test_descriptor_rejects_traversal_gaps_and_unknown_encoding(self):
        self.package()
        descriptor = self.output / "game/.disc-image"
        original = descriptor.read_bytes()
        for mutation in (lambda value: value.__setitem__(slice(PACKAGE.HEADER.size, PACKAGE.HEADER.size + 8), struct.pack("<Q", 1)),
                         lambda value: value.__setitem__(slice(PACKAGE.HEADER.size + 32, PACKAGE.HEADER.size + 36), struct.pack("<I", 77))):
            with self.subTest(mutation=mutation):
                data = bytearray(original)
                mutation(data)
                descriptor.write_bytes(data)
                with self.assertRaises(ValueError):
                    PACKAGE.read_descriptor(self.output / "game")
        descriptor.write_bytes(PACKAGE.HEADER.pack(PACKAGE.MAGIC, 1, 1) + PACKAGE.EXTENT.pack(0, 1, 0, 1, PACKAGE.RAW, 7) + b"../evil")
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            PACKAGE.read_descriptor(self.output / "game")

    def test_missing_metadata_and_corrupt_compressed_payload_rejected(self):
        self.package()
        _, extents = PACKAGE.read_descriptor(self.output / "game")
        extent = next(item for item in extents if item[4] == PACKAGE.ZLIB)
        payload = self.output / "game" / extent[5]
        with payload.open("r+b") as stream:
            stream.seek(extent[2])
            stream.write(b"\x00\x00")
        with self.assertRaises(Exception):
            list(PACKAGE.logical_blocks(self.output / "game"))
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.verify()

    def test_verify_cli_and_foreign_elf_cli_fail_without_traceback(self):
        self.package()
        result = subprocess.run([sys.executable, str(ROOT / "tools/package_resources.py"), "verify", str(self.output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Packaged ELF identity mismatch", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        result = subprocess.run([sys.executable, str(ROOT / "tools/package_resources.py"), "--iso", str(self.iso), "--output", str(self.base / "foreign")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("ELF SHA256 mismatch", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
