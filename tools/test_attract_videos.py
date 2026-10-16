#!/usr/bin/env python3
"""Bounded real-time menu -> original attract movies -> menu regression.

Requires the original packaged movie files. Only menu idle waits and unrelated
opening logos are omitted by the explicit exact-ELF diagnostic route.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import time


def audit_captures(report):
    for item in report.get('captures', []):
        path = Path(item['path'])
        item['exists'] = path.is_file()
        item['observed_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest() if item['exists'] else None
        item['hash_matches'] = item['exists'] and item['observed_sha256'] == item.get('sha256')
    return report


def validate(report):
    events = report['events']
    sequence = report.get('sequence', list(range(1, 16)))
    report['finished'] = [int(match.group(1)) for event in events if (match := re.search(r'\[movie\] finished DEMO(\d+)\.PSS', event['message'], re.IGNORECASE))]
    menu_pattern = r'menu responsive after (\d+) (?:completed demos|movie returns).*guest frames=60(?:\s|$)'
    report['returned_menu_after'] = [int(match.group(1)) for event in events if (match := re.search(menu_pattern, event['message']))]
    complete = any('completed all 15 demos and returned to menu' in event['message'] or f'completed attract sequence of {len(sequence)} movies and returned to menu' in event['message'] for event in events)
    report['failures'] = [event for event in events if re.search(r'playback failed|\[movie\] (skipped|stopped)', event['message'])]
    report['clips'] = []
    actual_order = []
    pending = None
    for event in events:
        if match := re.search(menu_pattern, event['message']):
            actual_order.append(('menu', int(match.group(1))))
        match = re.search(r'\[movie\] (starting|finished|skipped|stopped) (DEMO(\d+)\.PSS)(.*)', event['message'], re.IGNORECASE)
        if not match:
            continue
        action, filename, number, tail = match.groups()
        actual_order.append((action, int(number)))
        if action == 'starting':
            pending = {'name': filename.upper(), 'number': int(number), 'start_seconds': event['seconds']}
        elif pending:
            pending.update(outcome=action, end_seconds=event['seconds'], elapsed_seconds=event['seconds'] - pending['start_seconds'])
            pending['counters'] = {key: int(value) for key, value in re.findall(r'([\w-]+)=(\d+)', tail)}
            report['clips'].append(pending)
            pending = None
    expected_order = [('menu', 0)]
    for index, number in enumerate(sequence, 1):
        expected_order.extend([('starting', number), ('finished', number), ('menu', index)])
    report['sequence_order_verified'] = actual_order == expected_order
    report['passed'] = report['exit_code'] == 0 and not report['stalled'] and report['sequence_order_verified'] and complete and not report['failures']
    if 'assets' in report:
        expected_duration = {item['name']: item['duration_seconds'] for item in report['assets']}
        report['passed'] = report['passed'] and len(report['clips']) == len(sequence) and all(
            clip['outcome'] == 'finished' and clip['elapsed_seconds'] >= expected_duration[clip['name']] - 0.5
            and clip['counters'].get('decoded', 0) > 0 and clip['counters'].get('displayed', 0) > 0
            and clip['counters'].get('callback-source-frames', 0) > 0 for clip in report['clips'])
    if 'captures' in report:
        report['passed'] = report['passed'] and all(item['exists'] and item.get('hash_matches', False) for item in report['captures'])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/mac/night100'))
    parser.add_argument('--game', '--elf', type=Path, default=Path('out/night100/game'), help='Extracted disc root or its SLUS_203.49')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=1200)
    parser.add_argument('--analyze', action='store_true', help='Revalidate retained evidence without launching')
    parser.add_argument('--sequence', default=','.join(str(n) for n in range(1, 16)), help='Original DEMO numbers in playback order; use 3 for a targeted retry')
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--timeout must be finite and positive')
    output = args.output.resolve()
    if args.analyze:
        report = validate(audit_captures(json.loads((output / 'report.json').read_text())))
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({key: value for key, value in report.items() if key != 'events'}, indent=2))
        return 0 if report['passed'] else 1
    try:
        sequence = [int(n) for n in args.sequence.split(',')]
    except ValueError:
        parser.error('--sequence requires comma-separated DEMO numbers')
    if not sequence or len(set(sequence)) != len(sequence) or any(n < 1 or n > 15 for n in sequence):
        parser.error('--sequence requires distinct DEMO numbers 1..15')
    game = args.game.resolve()
    elf = game / 'SLUS_203.49' if game.is_dir() else game
    if not elf.is_file() or not args.binary.is_file():
        parser.error('Binary or game ELF is missing')
    assets = []
    for number in sequence:
        path = elf.parent / 'FMV' / 'DEMO' / f'DEMO{number}.PSS'
        if not path.is_file():
            parser.error(f'Original attract clip missing: {path}')
        probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'json', str(path)], capture_output=True, text=True, check=True, timeout=15)
        assets.append({'name': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'duration_seconds': float(json.loads(probe.stdout)['format']['duration'])})
    output.mkdir(parents=True, exist_ok=False)
    # Preserve precisely the tested executable while other work can rebuild.
    binary = output / 'night100-tested'
    shutil.copy2(args.binary.resolve(), binary)
    bundled_libraries = args.binary.resolve().parent / 'lib'
    if bundled_libraries.is_dir():
        (output / 'lib').symlink_to(bundled_libraries, target_is_directory=True)
    env = os.environ.copy()
    env.pop('NIGHT100_SKIP_VIDEOS', None)
    env.pop('NIGHT100_MOVIE_AUTO_SKIP_SECONDS', None)
    env['NIGHT100_ATTRACT_TEST'] = str(output)
    env['NIGHT100_ATTRACT_SEQUENCE'] = ','.join(str(n) for n in sequence)
    command = [str(binary), str(elf), '--seconds', str(args.timeout)]
    start = time.monotonic()
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    print(f'Attract playback PID {process.pid}, evidence {output}', flush=True)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    pending = b''
    events = []
    last_progress = start
    stalled = False
    clip_deadline = None
    watchdog_reason = None
    with (output / 'run.log').open('w') as log:
        while selector.get_map():
            now = time.monotonic()
            if process.poll() is None and (now - start > args.timeout + 15 or now - last_progress > 115 or (clip_deadline is not None and now > clip_deadline)):
                stalled = True
                watchdog_reason = 'Clip exceeded original duration plus 30s' if clip_deadline is not None and now > clip_deadline else 'No movie/menu progress within 115s or overall deadline exceeded'
                try:
                    subprocess.run(['sample', str(process.pid), '3', '-file', str(output / 'stalled.sample.txt')], capture_output=True, timeout=15)
                except (OSError, subprocess.TimeoutExpired):
                    pass
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                print('Attract route stalled; process sample retained', flush=True)
            for key, _ in selector.select(timeout=1):
                data = os.read(key.fd, 65536)
                if not data:
                    selector.unregister(key.fileobj)
                    continue
                pending += data
                while b'\n' in pending:
                    raw, pending = pending.split(b'\n', 1)
                    line = raw.decode(errors='replace')
                    elapsed = time.monotonic() - start
                    log.write(f'{elapsed:9.3f} {line}\n')
                    log.flush()
                    if '[movie]' in line or '[attract-test]' in line:
                        events.append({'seconds': elapsed, 'message': line})
                        print(f'{elapsed:8.2f} {line}', flush=True)
                        last_progress = time.monotonic()
                        if match := re.search(r'\[movie\] starting DEMO(\d+)\.PSS', line, re.IGNORECASE):
                            name = f'DEMO{int(match.group(1))}.PSS'
                            duration = next(item['duration_seconds'] for item in assets if item['name'] == name)
                            clip_deadline = time.monotonic() + duration + 30
                        elif re.search(r'\[movie\] (finished|skipped|stopped)|playback failed', line):
                            clip_deadline = None
    code = process.wait(timeout=10)
    report = validate(audit_captures({'exit_code': code, 'stalled': stalled, 'elapsed_seconds': time.monotonic() - start,
              'watchdog_reason': watchdog_reason, 'sequence': sequence, 'assets': assets,
              'captures': [{'path': str(path), 'exists': path.is_file(), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None} for path in
                           [*(output / f'menu-after-{n}.png' for n in range(len(sequence) + 1)), *(output / f'demo-{n}.png' for n in sequence)]],
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'command': command, 'events': events}))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'events'}, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
