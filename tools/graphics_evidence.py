#!/usr/bin/env python3
"""Record local provenance for a deterministic GS replay comparison."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
trace = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

sources = [root / 'CMakeLists.txt', root / 'dependencies.json']
for directory in ('src/graphics', 'shaders', 'tests/graphics', 'patches'):
    sources.extend(p for p in (root / directory).rglob('*') if p.is_file())
report = {
    'version': 1,
    'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
    'tracked_tree_dirty': bool(subprocess.check_output(['git', 'diff', '--name-only', 'HEAD'], cwd=root)),
    'trace': str(trace),
    'trace_sha256': digest(trace),
    'sources_sha256': {str(p.relative_to(root)): digest(p) for p in sorted(sources)},
    'artifacts_sha256': {p.name: digest(p) for p in sorted(output.iterdir()) if p.is_file() and p.name != 'evidence.json'},
    'reference': 'Existing GSCpuBackend; not independent PS2 hardware',
}
(output / 'evidence.json').write_text(json.dumps(report, indent=2) + '\n')
print('Saved replay provenance:', output / 'evidence.json')
