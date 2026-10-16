#!/usr/bin/env python3
"""Symbolize the bounded Switch native CPU sampler against its matching ELF."""
import argparse
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess


def parse_profile(text):
    match = re.search(r'^\[cpu-native\] reason=(.*?)\n\[cpu-native\] csv_hex=pc,lr,sp\n(.*?)^\[cpu-native\] end$', text, re.M | re.S)
    if not match:
        raise ValueError('No complete native CPU profile; retain its header, CSV and end marker')
    metadata = dict(item.split('=', 1) for item in ('reason=' + match[1]).split())
    rows = []
    for line in match[2].splitlines():
        if not re.fullmatch(r'[0-9a-fA-F]+,[0-9a-fA-F]+,[0-9a-fA-F]+', line):
            raise ValueError('Malformed or interleaved native sample row')
        rows.append(tuple(int(value, 16) for value in line.split(',')))
    if len(rows) != int(metadata['samples']):
        raise ValueError('Native sample count differs from its header')
    if not rows:
        raise ValueError('Native sampler collected no program counters: ' + metadata['reason'])
    return metadata, rows


def parse_symbols(text):
    symbols = []
    for line in text.splitlines():
        fields = line.split(maxsplit=3)
        if len(fields) == 4 and fields[2].lower() in ('t', 'w'):
            symbols.append((int(fields[0], 16), int(fields[1], 16), fields[3]))
        elif len(fields) == 3 and fields[1].lower() in ('t', 'w'):
            # libnx assembly SVC stubs often have no ELF .size directive.
            symbols.append((int(fields[0], 16), 0, fields[2]))
    symbols.sort()
    for index in range(len(symbols) - 1):
        address, size, name = symbols[index]
        if not size:
            symbols[index] = (address, symbols[index + 1][0] - address, name)
    return symbols


def summarize(metadata, samples, symbols):
    anchors = [address for address, _, name in symbols if name == 'night100SwitchCpuSamplerAnchor']
    if len(anchors) != 1:
        raise ValueError('Matching ELF must define one night100SwitchCpuSamplerAnchor')
    base = int(metadata['anchor'], 16) - anchors[0]
    if base < 0:
        raise ValueError('Runtime anchor precedes its ELF address')
    starts = [symbol[0] for symbol in symbols]

    def locate(address):
        relative = address - base
        index = bisect.bisect_right(starts, relative) - 1
        if index >= 0:
            start, size, name = symbols[index]
            if size and start <= relative < start + size:
                return name
        return '<outside known ELF functions>'

    def histogram(column):
        counts = Counter(locate(sample[column]) for sample in samples)
        return [{'function': name, 'samples': count, 'percent': round(count * 100 / len(samples), 3)}
                for name, count in counts.most_common()]

    return {'samples': len(samples), 'load_base': hex(base), 'sampler': metadata,
            'scope': 'Game-thread state samples during eligible controllable gameplay while EE executes and the observed GPU stage is idle. Blocking syscalls may be sampled; percentages are observations, not CPU-time shares.',
            'program_counters': histogram(0), 'return_addresses': histogram(1)}


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, default=Path('build/switch/night100.elf'))
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nm', default='/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm')
    args = parser.parse_args()
    metadata, samples = parse_profile(args.log.read_text())
    raw = subprocess.check_output([args.nm, '--numeric-sort', '--print-size', '--defined-only', '--demangle', str(args.elf)], text=True)
    report = summarize(metadata, samples, parse_symbols(raw))
    report['elf_sha256'] = digest(args.elf)
    report['log_sha256'] = digest(args.log)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    for item in report['program_counters'][:20]:
        print(f"{item['percent']:6.2f}% {item['samples']:4d} {item['function']}")
    print('Evidence:', args.output)


if __name__ == '__main__':
    main()
