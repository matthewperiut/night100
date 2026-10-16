#!/usr/bin/env python3
"""Build one-copy loose resources plus a lossless compact logical disc.

  python3 tools/package_resources.py --iso GAME.iso --output PACKAGE
  python3 tools/package_resources.py verify PACKAGE

The compact reader reconstructs every ISO byte through game/.disc-image. Files
are stored once under game; proven-zero gaps are synthesized and other gaps use
independent <=1 MiB zlib blocks. Verification hashes the reconstructed stream
without creating another ISO. Outputs never replace existing directories.
"""
import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import sys
import tempfile
import zlib

ELF_SHA256 = "d5febfa97119fba2c2f3eb58abaae268037afae2773ddc57cab7e1c813b485b1"
BLOCK = 1024 * 1024
SECTOR = 2048
MAGIC = b"N100DSC1"
HEADER = struct.Struct("<8sQI")
EXTENT = struct.Struct("<QQQQII")
RAW, ZERO, ZLIB = 0, 1, 2
GAPS = ".disc-data/gaps.bin"
DESCRIPTOR = ".disc-image"
MANIFEST = "resource-manifest.json"
MAX_MANIFEST = 32 * 1024 * 1024


def safe_path(name):
    if not isinstance(name, str) or not name or len(name.encode("utf-8")) > 4096:
        raise ValueError("Invalid resource path length")
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts) or name.startswith("/") or any(ord(c) < 32 or ord(c) == 127 or c in "\\:" for c in name):
        raise ValueError(f"Unsafe resource path: {name!r}")
    return PurePosixPath(name)


def contained(root, name):
    relative = safe_path(name)
    path = root.joinpath(*relative.parts)
    cursor = root
    for component in relative.parts:
        cursor = cursor / component
        if cursor.is_symlink():
            raise ValueError(f"Resource symlink is forbidden: {name}")
    return path


def exact(stream, count):
    data = stream.read(count)
    if len(data) != count:
        raise ValueError("Truncated disc/resource data")
    return data


def both32(record, offset):
    value = struct.unpack_from("<I", record, offset)[0]
    if value != struct.unpack_from(">I", record, offset + 4)[0]:
        raise ValueError("Inconsistent ISO9660 dual-endian field")
    return value


def index_iso(iso):
    """Strict ISO9660 walk: preserve original physical extents, reject aliases."""
    size = iso.stat().st_size
    result, names, directories = {}, {}, set()
    with iso.open("rb") as stream:
        stream.seek(16 * SECTOR)
        primary = exact(stream, SECTOR)
        if primary[:7] != b"\x01CD001\x01" or struct.unpack_from("<H", primary, 128)[0] != SECTOR or struct.unpack_from(">H", primary, 130)[0] != SECTOR:
            raise ValueError("Expected 2048-byte ISO9660 primary volume descriptor")
        if both32(primary, 80) * SECTOR > size:
            raise ValueError("ISO9660 volume exceeds input file")
        root_length = primary[156]
        root = primary[156:156 + root_length]
        if len(root) < 34:
            raise ValueError("Invalid ISO9660 root record")

        def walk(lbn, length, prefix, depth):
            if depth > 64 or lbn in directories or length > 64 * 1024 * 1024 or lbn * SECTOR + length > size:
                raise ValueError("Invalid, repeated or cyclic ISO9660 directory")
            directories.add(lbn)
            stream.seek(lbn * SECTOR)
            data, pos = exact(stream, length), 0
            while pos < len(data):
                record_length = data[pos]
                if record_length == 0:
                    pos = (pos // SECTOR + 1) * SECTOR
                    continue
                if record_length < 34 or pos % SECTOR + record_length > SECTOR or pos + record_length > len(data):
                    raise ValueError("Truncated or straddling ISO9660 directory record")
                record = data[pos:pos + record_length]
                pos += record_length
                name_length = record[32]
                if not name_length or 33 + name_length > len(record):
                    raise ValueError("Invalid ISO9660 filename length")
                raw = record[33:33 + name_length]
                if raw in (b"\x00", b"\x01"):
                    continue
                name = raw.decode("ascii")
                if ";" in name:
                    name, version = name.rsplit(";", 1)
                    if version != "1":
                        raise ValueError("Unsupported ISO9660 file version")
                if "/" in name:
                    raise ValueError("ISO9660 filename contains a path separator")
                path = prefix + name
                safe_path(path)
                key = path.casefold()
                if key in names or path.split("/")[0].casefold() in {DESCRIPTOR, ".disc-map", ".disc-data"}:
                    raise ValueError(f"Case, duplicate or reserved-path collision: {path}")
                names[key] = path
                start, count = both32(record, 2), both32(record, 10)
                if record[25] & 128 or record[26] or record[27] or start * SECTOR + count > size:
                    raise ValueError("Unsupported or out-of-range ISO9660 file extent")
                if record[25] & 2:
                    walk(start, count, path + "/", depth + 1)
                else:
                    result[path] = {"lbn": start, "size": count}

        walk(both32(root, 2), both32(root, 10), "", 0)
    # The reader maps physical bytes into one backing asset. Ambiguous overlaps
    # must never be silently discarded or duplicated.
    end = 0
    for name, entry in sorted(result.items(), key=lambda item: (item[1]["lbn"], item[0])):
        start = entry["lbn"] * SECTOR
        if entry["size"] and start < end:
            raise ValueError(f"Overlapping ISO9660 asset extents: {name}")
        end = max(end, start + entry["size"])
    return result


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(BLOCK), b""):
            sha.update(block)
    return sha.hexdigest()


def extent_digest(stream, offset, length):
    sha = hashlib.sha256()
    stream.seek(offset)
    while length:
        data = exact(stream, min(BLOCK, length))
        sha.update(data)
        length -= len(data)
    return sha.hexdigest()


def publish(source, destination):
    """Atomic directory rename with kernel-enforced no-replace semantics."""
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        function = libc.renamex_np
        function.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        result = function(os.fsencode(source), os.fsencode(destination), 0x4)  # RENAME_EXCL
    elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
        function = libc.renameat2
        function.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        result = function(-100, os.fsencode(source), -100, os.fsencode(destination), 1)  # RENAME_NOREPLACE
    else:
        raise OSError("Atomic no-replace directory publication is unsupported on this platform")
    if result:
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            raise FileExistsError(error, os.strerror(error), str(destination))
        raise OSError(error, os.strerror(error), str(destination))


def package(iso, output, *, expected_elf_sha256=ELF_SHA256):
    iso, output = Path(iso).resolve(), Path(output).absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Output already exists: {output}")
    entries = index_iso(iso)
    if "SLUS_203.49" not in entries:
        raise ValueError("Disc lacks the required SLUS_203.49 ELF")
    initial_stat = iso.stat()
    canonical, hashes, content = {}, {}, {}
    ordered = sorted(entries.items(), key=lambda item: (item[1]["lbn"], item[0]))
    with iso.open("rb") as stream:
        elf = entries["SLUS_203.49"]
        if extent_digest(stream, elf["lbn"] * SECTOR, elf["size"]) != expected_elf_sha256:
            raise ValueError("ELF SHA256 mismatch: this build requires the exact supported USA executable")
        # Hash before choosing backing paths, so duplicate file names remain
        # available through internal aliases and every logical extent maps to
        # exactly one canonical payload. PAD is never chosen as a loose asset.
        for name, entry in ordered:
            hashes[name] = extent_digest(stream, entry["lbn"] * SECTOR, entry["size"])
            key = (entry["size"], hashes[name])
            if name != "PAD.PAD":
                canonical[name] = content.setdefault(key, name)
        if "PAD.PAD" in entries:
            pad = entries["PAD.PAD"]
            canonical["PAD.PAD"] = content.get((pad["size"], hashes["PAD.PAD"]))
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}.resources-", dir=output.parent))
    try:
        game = stage / "game"
        (game / ".disc-data").mkdir(parents=True)
        extents, assets, aliases, deduplicated = [], [], [], []
        source_sha = hashlib.sha256()
        source_size = iso.stat().st_size
        gap_logical = gap_zero = gap_stored = 0

        def append(offset, length, file_offset, stored, encoding, path):
            if not length:
                return
            if encoding == ZERO and extents and extents[-1][4] == ZERO and extents[-1][0] + extents[-1][1] == offset:
                old = extents[-1]
                extents[-1] = (old[0], old[1] + length, 0, 0, ZERO, "")
            else:
                extents.append((offset, length, file_offset, stored, encoding, path))

        with iso.open("rb") as source, (game / GAPS).open("wb") as payload:
            cursor = 0

            def gap(stop):
                nonlocal cursor, gap_logical, gap_zero, gap_stored
                while cursor < stop:
                    data = exact(source, min(BLOCK, stop - cursor))
                    source_sha.update(data)
                    gap_logical += len(data)
                    if data.count(0) == len(data):
                        append(cursor, len(data), 0, 0, ZERO, "")
                        gap_zero += len(data)
                    else:
                        compressed = zlib.compress(data, 6)
                        encoding = ZLIB if len(compressed) < len(data) else RAW
                        stored = compressed if encoding == ZLIB else data
                        append(cursor, len(data), payload.tell(), len(stored), encoding, GAPS)
                        payload.write(stored)
                        gap_stored += len(stored)
                    cursor += len(data)

            for name, entry in ordered:
                backing = canonical.get(name)
                if name == "PAD.PAD" and backing is None:
                    continue
                start, length = entry["lbn"] * SECTOR, entry["size"]
                if length:
                    gap(start)
                asset_sha = hashlib.sha256()
                target = None
                if backing == name:
                    path = contained(game, name)
                    path.parent.mkdir(parents=True, exist_ok=True)
                    target = path.open("xb")
                try:
                    remaining = length
                    while remaining:
                        data = exact(source, min(BLOCK, remaining))
                        source_sha.update(data)
                        asset_sha.update(data)
                        if target is not None:
                            target.write(data)
                        remaining -= len(data)
                finally:
                    if target is not None:
                        target.close()
                if asset_sha.hexdigest() != hashes[name]:
                    raise ValueError(f"ISO content changed while packaging: {name}")
                append(start, length, 0, length, RAW, backing)
                if name != backing:
                    deduplicated.append({"path": name, "canonical": backing, "size": length, "sha256": hashes[name]})
                if name != "PAD.PAD":
                    if name != backing:
                        path = contained(game, name)
                        path.parent.mkdir(parents=True, exist_ok=True)
                        relative = os.path.relpath(contained(game, backing), path.parent)
                        path.symlink_to(relative)
                        aliases.append({"path": "game/" + name, "target": relative, "canonical": "game/" + backing,
                                        "size": length, "sha256": hashes[name]})
                    assets.append({"path": "game/" + name, "lbn": entry["lbn"], "size": length, "sha256": hashes[name]})
                if length:
                    cursor = start + length
            gap(source_size)
            if source.read(1):
                raise ValueError("ISO changed size while packaging")
        final_stat = iso.stat()
        if (initial_stat.st_size, initial_stat.st_mtime_ns, initial_stat.st_ino) != (final_stat.st_size, final_stat.st_mtime_ns, final_stat.st_ino):
            raise ValueError("ISO changed while packaging")
        with (game / DESCRIPTOR).open("xb") as stream:
            stream.write(HEADER.pack(MAGIC, source_size, len(extents)))
            for offset, length, file_offset, stored, encoding, path in extents:
                encoded = path.encode("utf-8")
                stream.write(EXTENT.pack(offset, length, file_offset, stored, encoding, len(encoded)))
                stream.write(encoded)
        (game / ".disc-map").write_text("".join(f'{name.lower()}\t{entry["lbn"]}\t{entry["size"]}\n' for name, entry in sorted(entries.items())))
        files = [{"path": path.relative_to(stage).as_posix(), "size": path.stat().st_size, "sha256": digest(path)}
                 for path in sorted(game.rglob("*")) if path.is_file() and not path.is_symlink()]
        manifest = {"schema": 1, "format": MAGIC.decode("ascii"),
                    "elf": {"path": "game/SLUS_203.49", "sha256": expected_elf_sha256},
                    "logical_disc": {"path": "game/" + DESCRIPTOR, "size": source_size, "sha256": source_sha.hexdigest()},
                    "assets": sorted(assets, key=lambda entry: entry["path"]), "files": files,
                    "aliases": sorted(aliases, key=lambda entry: entry["path"]), "deduplicated": deduplicated,
                    "omitted_loose_files": ["PAD.PAD"] if "PAD.PAD" in entries else [],
                    "gaps": {"logical_bytes": gap_logical, "proven_zero_bytes": gap_zero, "stored_bytes": gap_stored},
                    "resource_bytes": sum(entry["size"] for entry in files)}
        (stage / MANIFEST).write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n")
        # A full independent read of the produced package catches mapping,
        # padding, compression, short-copy and source-identity mistakes before
        # publishing. This allocates at most one decompressed block at a time.
        verify(stage, expected_elf_sha256=expected_elf_sha256)
        publish(stage, output)
        return manifest
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def read_descriptor(game):
    path = game / DESCRIPTOR
    if path.stat().st_size > MAX_MANIFEST:
        raise ValueError("Compact disc descriptor exceeds size limit")
    extents, cursor = [], 0
    with path.open("rb") as stream:
        magic, size, count = HEADER.unpack(exact(stream, HEADER.size))
        if magic != MAGIC or not size or not 0 < count <= 1000000:
            raise ValueError("Invalid compact disc descriptor header")
        for _ in range(count):
            offset, length, file_offset, stored, encoding, path_length = EXTENT.unpack(exact(stream, EXTENT.size))
            if not length or offset != cursor or length > size - cursor or path_length > 4096:
                raise ValueError("Noncontiguous or invalid compact disc extent")
            name = exact(stream, path_length).decode("utf-8")
            if encoding == ZERO:
                if name or file_offset or stored:
                    raise ValueError("Invalid zero extent")
            elif encoding in (RAW, ZLIB):
                backing = contained(game, name)
                if not backing.is_file() or file_offset + stored > backing.stat().st_size:
                    raise ValueError("Compact disc backing file is missing or truncated")
                if encoding == RAW and stored != length or encoding == ZLIB and (length > BLOCK or not 0 < stored <= 2 * BLOCK):
                    raise ValueError("Invalid raw/compressed extent size")
            else:
                raise ValueError("Unknown compact disc extent encoding")
            extents.append((offset, length, file_offset, stored, encoding, name))
            cursor += length
        if cursor != size or stream.read(1):
            raise ValueError("Incomplete or trailing compact disc descriptor")
    return size, extents


def logical_blocks(game):
    _, extents = read_descriptor(game)
    zero = bytes(BLOCK)
    for _, length, offset, stored, encoding, name in extents:
        if encoding == ZERO:
            while length:
                count = min(length, BLOCK)
                yield zero[:count]
                length -= count
        else:
            with contained(game, name).open("rb") as stream:
                stream.seek(offset)
                if encoding == ZLIB:
                    compressed = exact(stream, stored)
                    decoder = zlib.decompressobj()
                    data = decoder.decompress(compressed, length + 1)
                    if len(data) != length or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                        raise ValueError("Invalid compressed disc gap")
                    yield data
                else:
                    while length:
                        data = exact(stream, min(length, BLOCK))
                        yield data
                        length -= len(data)


def verify(package_root, *, expected_elf_sha256=ELF_SHA256):
    root = Path(package_root)
    with (root / MANIFEST).open() as stream:
        manifest = json.load(stream)
    if manifest.get("schema") != 1 or manifest.get("format") != MAGIC.decode("ascii") or manifest.get("logical_disc", {}).get("path") != "game/" + DESCRIPTOR:
        raise ValueError("Unsupported resource manifest")
    seen = set()
    recorded = {}
    for entry in manifest["files"]:
        name = entry["path"]
        path = contained(root, name)
        if name.casefold() in seen:
            raise ValueError("Duplicate resource manifest path")
        seen.add(name.casefold())
        recorded[name] = entry
        if not path.is_file() or path.stat().st_size != entry["size"] or digest(path) != entry["sha256"]:
            raise ValueError(f"Resource checksum mismatch: {name}")
    if "game/" + DESCRIPTOR not in seen or "game/slus_203.49" not in seen:
        raise ValueError("Required resource files absent from manifest")
    for alias in manifest.get("aliases", []):
        name, target, canonical_name = alias["path"], alias["target"], alias["canonical"]
        relative = safe_path(name)
        if name.casefold() in seen or not name.startswith("game/") or os.path.isabs(target):
            raise ValueError("Invalid resource alias")
        seen.add(name.casefold())
        path = contained(root, relative.parent.as_posix()) / relative.name
        canonical_path = contained(root, canonical_name)
        if not path.is_symlink() or os.readlink(path) != target or path.resolve(strict=True) != canonical_path.resolve(strict=True):
            raise ValueError(f"Resource alias mismatch: {name}")
        if canonical_name not in recorded or not canonical_name.startswith("game/") or alias["sha256"] != recorded[canonical_name]["sha256"] or alias["size"] != recorded[canonical_name]["size"]:
            raise ValueError("Alias does not refer to a declared canonical asset")
    elf = manifest["elf"]
    if elf["path"] != "game/SLUS_203.49" or elf["sha256"] != expected_elf_sha256 or digest(contained(root, elf["path"])) != elf["sha256"]:
        raise ValueError("Packaged ELF identity mismatch")
    logical = hashlib.sha256()
    total = 0
    for block in logical_blocks(root / "game"):
        logical.update(block)
        total += len(block)
    if total != manifest["logical_disc"]["size"] or logical.hexdigest() != manifest["logical_disc"]["sha256"]:
        raise ValueError("Reconstructed logical ISO SHA256 mismatch")
    return {"schema": 1, "verified": True, "logical_bytes": total, "logical_sha256": logical.hexdigest(),
            "resource_bytes": manifest["resource_bytes"], "asset_count": len(manifest["assets"])}


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "verify":
        parser = argparse.ArgumentParser(description="Stream-verify packaged resources and every reconstructed ISO byte")
        parser.add_argument("package", type=Path)
        args = parser.parse_args(arguments[1:])
        action = lambda: verify(args.package)
    else:
        parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
        parser.add_argument("--iso", type=Path, required=True)
        parser.add_argument("--output", type=Path, required=True)
        args = parser.parse_args(arguments)
        action = lambda: package(args.iso, args.output)
    try:
        result = action()
    except (OSError, ValueError, KeyError, TypeError, struct.error, zlib.error) as error:
        parser.exit(2, f"package_resources: {error}\n")
    if "assets" in result:
        result = {"output": str(args.output.absolute()), "asset_count": len(result["assets"]),
                  "resource_bytes": result["resource_bytes"], "logical_disc": result["logical_disc"],
                  "aliases": len(result["aliases"])}
    print(json.dumps(result, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
