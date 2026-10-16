#!/usr/bin/env python3
"""Check a captured, settled title sequence for erased or flickering frames.

Capture with NIGHT100_CAPTURE_COMPLETED=logs/completed ./night100 smoke
--seconds 12 --skip-videos. The bridge records game frames 300 through 323.
This checks temporal stability, not full native/PS2 rendering parity.
"""
import hashlib
import json
import math
from pathlib import Path
import sys

directory = Path(sys.argv[1])
frames = []
for path in sorted(directory.glob('*.ppm')):
    magic, size, maximum, data = path.read_bytes().split(b'\n', 3)
    width, height = map(int, size.split())
    if magic != b'P6' or maximum != b'255' or width != 640 or height < 440:
        raise SystemExit(f'FAIL: invalid title capture {path}')
    data = data[:width * height * 3]
    if len(data) != width * height * 3:
        raise SystemExit(f'FAIL: truncated title capture {path}')
    logo = b''.join(data[(y * width + 80) * 3:(y * width + 560) * 3]
                    for y in range(20, 240))
    frames.append((data, logo))

bright = [sum(max(logo[i:i+3]) > 40 for i in range(0, len(logo), 3))
          for _, logo in frames]
differences = [math.sqrt(sum((a-b)**2 for a, b in zip(x[1], y[1])) / len(x[1]))
               for x, y in zip(frames, frames[1:])]
report = {
    'frames': len(frames),
    'minimum_logo_pixels_above_40': min(bright, default=0),
    'maximum_consecutive_logo_rgb_rmse': max(differences, default=0),
    'distinct_complete_frames': len({hashlib.sha256(data).hexdigest() for data, _ in frames}),
}
report['passed'] = (len(frames) == 24 and report['minimum_logo_pixels_above_40'] > 10000
                    and report['maximum_consecutive_logo_rgb_rmse'] <= 1
                    and report['distinct_complete_frames'] > 1)
(directory / 'stability.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['passed'] else 1)
