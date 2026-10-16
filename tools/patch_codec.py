"""Container for night100.patch.

The patch holds the Switch executable, compressed and encrypted with a key taken
from the game's own SLUS_203.49 executable. Without the matching executable from a
disc image it cannot be opened, and any other version fails the integrity check.
Standard library only.
"""
import hashlib
import hmac
import lzma
import struct

MAGIC = b'N100PTCH'
VERSION = 1
HEADER = struct.Struct('<8sB16s16sQ32s')  # magic, version, salt, nonce, raw size, raw sha256
TAG_SIZE = 32


class PatchError(Exception):
    pass


def _keys(elf, salt):
    h = hashlib.blake2b(key=salt, digest_size=64)
    h.update(b'night100 patch v1\0')
    h.update(struct.pack('<Q', len(elf)))
    h.update(elf)
    master = h.digest()
    return master[:32], master[32:]


def _crypt(key, nonce, data):
    out = bytearray(len(data))
    counter = 0
    step = 1 << 20
    for pos in range(0, len(data), step):
        chunk = data[pos:pos + step]
        blocks = (len(chunk) + 63) // 64
        stream = b''.join(
            hashlib.blake2b(nonce + struct.pack('<Q', counter + i), key=key, digest_size=64).digest()
            for i in range(blocks))[:len(chunk)]
        counter += blocks
        mixed = int.from_bytes(chunk, 'little') ^ int.from_bytes(stream, 'little')
        out[pos:pos + len(chunk)] = mixed.to_bytes(len(chunk), 'little')
    return bytes(out)


def seal(payload, elf, salt, nonce):
    """payload: the executable bytes. elf: the game's SLUS_203.49 bytes."""
    enc_key, mac_key = _keys(elf, salt)
    body = _crypt(enc_key, nonce, lzma.compress(payload, preset=6))
    header = HEADER.pack(MAGIC, VERSION, salt, nonce, len(payload), hashlib.sha256(payload).digest())
    tag = hmac.new(mac_key, header + body, hashlib.sha256).digest()
    return header + body + tag


def unseal(patch, elf):
    """Returns the executable bytes, or raises PatchError."""
    if len(patch) < HEADER.size + TAG_SIZE:
        raise PatchError('the patch file is truncated')
    magic, version, salt, nonce, size, digest = HEADER.unpack_from(patch)
    if magic != MAGIC or version != VERSION:
        raise PatchError('this is not a night100 patch file, or it is from a different version')
    body = patch[HEADER.size:-TAG_SIZE]
    enc_key, mac_key = _keys(elf, salt)
    expected = hmac.new(mac_key, patch[:HEADER.size] + body, hashlib.sha256).digest()
    if not hmac.compare_digest(expected, patch[-TAG_SIZE:]):
        raise PatchError('it does not match this game executable')
    try:
        payload = lzma.decompress(_crypt(enc_key, nonce, body))
    except lzma.LZMAError as error:
        raise PatchError(f'the patch data is damaged ({error})') from error
    if len(payload) != size or hashlib.sha256(payload).digest() != digest:
        raise PatchError('the patch data is damaged')
    return payload
