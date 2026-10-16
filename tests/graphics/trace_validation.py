#!/usr/bin/env python3
"""A broken or empty capture must never produce a passing parity result."""
import gzip
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
trace = Path(sys.argv[2])
with trace.open('rb') as stream:
    compressed = stream.read(2) == b'\x1f\x8b'
with (gzip.open(trace, 'rb') if compressed else trace.open('rb')) as stream:
    header = stream.read(24)
assert len(header) == 24
cases = {
    'bad-header': b'invalid',
    'truncated-payload': header + struct.pack('<II', 3, 312) + bytes(8),
    'unclosed-stream': header,
    'empty-stream': header + struct.pack('<II', 0, 0),
    'truncated-gzip': gzip.compress(header + struct.pack('<II', 0, 0))[:-8],
}
with tempfile.TemporaryDirectory(prefix='night100-trace-check-') as temporary:
    root = Path(temporary)
    for name, data in cases.items():
        path = root / name
        path.write_bytes(data)
        result = subprocess.run([str(binary), str(path), str(root / (name + '-output'))], capture_output=True, text=True, timeout=20)
        if result.returncode == 0:
            raise SystemExit('FAIL: invalid trace passed: ' + name)
        if name == 'truncated-gzip' and 'gzip' not in result.stderr:
            raise SystemExit('FAIL: gzip truncation not detected: ' + result.stderr)
print('PASS: 5 malformed, truncated, or empty GS captures rejected')
