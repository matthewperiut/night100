#!/usr/bin/env python3
"""Keep unchanged generated files' timestamps to make iterative builds cheap."""
from pathlib import Path
import shutil

root = Path(__file__).resolve().parent.parent
source, destination = root / 'build/generated-stage', root / 'generated'
destination.mkdir(exist_ok=True)
names = set()
changed = 0
for path in source.iterdir():
    if not path.is_file():
        continue
    names.add(path.name)
    target = destination / path.name
    if not target.exists() or path.read_bytes() != target.read_bytes():
        shutil.copy2(path, target)
        changed += 1
for path in destination.iterdir():
    if path.is_file() and path.name not in names:
        path.unlink()
print(f'Updated {changed} of {len(names)} generated files.')
