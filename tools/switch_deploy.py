#!/usr/bin/env python3
"""Deploy changed NRO/resources without touching unrelated SD files or logs."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time
import urllib.error
from switch_remote import Client, digest, remote_path
from package_resources import contained as local_path, safe_path


def upload_verified(client, source, destination, expected=None):
    expected = expected or digest(source)
    try:
        current = client.hash(destination)
        if current['hash'] == expected and current['size'] == source.stat().st_size:
            return False
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
    client.upload(source, destination)
    current = client.hash(destination)
    if current['hash'] != expected or current['size'] != source.stat().st_size:
        raise ValueError(f'Upload verification failed: {destination}')
    return True


def selected_assets(package, manifest, selection):
    if not isinstance(manifest, dict) or manifest.get('format') != 'night100-switch-assets-v1' or type(manifest.get('schema')) is not int or manifest['schema'] != 1:
        raise ValueError('Unsupported Switch asset manifest')
    if selection not in ('bootstrap', 'all') or not isinstance(manifest.get('files'), list):
        raise ValueError('Invalid Switch asset selection or file list')
    selected, seen = [], set()
    for item in manifest['files']:
        if not isinstance(item, dict):
            raise ValueError('Invalid Switch asset entry')
        relative = safe_path(item.get('path'))
        if len(relative.parts) < 2 or relative.parts[0] != 'game' or str(relative).casefold() in seen:
            raise ValueError('Duplicate or unsafe Switch asset path')
        seen.add(str(relative).casefold())
        if type(item.get('bytes')) is not int or item['bytes'] < 0 or type(item.get('bootstrap')) is not bool or not isinstance(item.get('sha256'), str) or not re.fullmatch(r'[0-9a-f]{64}', item['sha256']):
            raise ValueError('Invalid Switch asset metadata')
        source = local_path(package, str(relative))
        if selection == 'all' or item['bootstrap']:
            # Validate the whole selected set before the first remote write.
            if not source.is_file() or source.stat().st_size != item['bytes'] or digest(source) != item['sha256']:
                raise ValueError(f'Local asset differs from manifest: {relative}')
            selected.append(item)
    return selected


def deploy_assets(client, package, root, selection):
    manifest = json.loads((package / 'asset-manifest.json').read_text())
    expected = selected_assets(package, manifest, selection)
    record_path = remote_path(root, 'deployment.json')
    try:
        record = json.loads(client.read(record_path))
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        record = {'schema': 1, 'files': {}}
    if not isinstance(record, dict) or record.get('schema') != 1 or not isinstance(record.get('files'), dict):
        raise ValueError('Unsupported remote deployment record')
    files = record['files']
    # Make the ELF/system map and boot resources available before later areas.
    expected.sort(key=lambda item: (not item['bootstrap'], item['path'] != 'game/SLUS_203.49', item['path']))
    changed = skipped = bytes_sent = 0
    for item in expected:
        relative = item['path']
        destination = remote_path(root, relative)
        if files.get(relative) == {'sha256': item['sha256'], 'bytes': item['bytes']}:
            skipped += 1
            continue
        source = package / relative
        uploaded = upload_verified(client, source, destination, item['sha256'])
        files[relative] = {'sha256': item['sha256'], 'bytes': item['bytes']}
        changed += int(uploaded)
        bytes_sent += item['bytes'] if uploaded else 0
        print(f'{"Uploaded" if uploaded else "Verified"} {relative} ({item["bytes"]:,} bytes)', flush=True)
        if changed % 4 == 0:
            save_record(client, record, record_path)
    record['asset_manifest_sha256'] = digest(package / 'asset-manifest.json')
    record['complete'] = all(files.get(item['path']) == {'sha256': item['sha256'], 'bytes': item['bytes']}
                             for item in manifest['files'])
    save_record(client, record, record_path)
    upload_verified(client, package / 'asset-manifest.json', remote_path(root, 'asset-manifest.json'))
    return {'uploaded': changed, 'unchanged': skipped, 'uploaded_bytes': bytes_sent, 'complete': record['complete']}


def save_record(client, record, path):
    with tempfile.TemporaryDirectory(prefix='night100-switch-deploy-') as temporary:
        source = Path(temporary) / 'deployment.json'
        source.write_text(json.dumps(record, sort_keys=True) + '\n')
        client.upload(source, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host')
    parser.add_argument('--package', type=Path, default=Path('out/switch/night100'))
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--root', default='/switch/night100')
    parser.add_argument('--assets', choices=['none', 'bootstrap', 'all'], default='none')
    parser.add_argument('--assets-only', action='store_true')
    parser.add_argument('--launch', action='store_true', help='From visible hbmenu: open NetLoader and send the NRO once')
    parser.add_argument('--output', type=Path, help='Retain deployment/run evidence in a fresh directory')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--skip-videos', action='store_true')
    parser.add_argument('--gpu-self-test', action='store_true')
    parser.add_argument('--cpu-profile', action='store_true', help='Sample game-thread CPU work on one frame in 60')
    parser.add_argument('--native-cpu-profile', action='store_true', help='Collect a bounded native CPU profile during controllable gameplay')
    parser.add_argument('--stall-watchdog', action='store_true', help='Arm the separate fsp-srv stall watchdog (diagnostic; hung the console once with --native-cpu-profile)')
    parser.add_argument('--cpu-kernel-benchmark', action='store_true', help='Compare original/native animation kernels before game startup')
    parser.add_argument('--replay-session', type=int, help='Restore and play a numbered Switch recording')
    parser.add_argument('--aspect', choices=['4:3', '16:9'], default='16:9', help='Forwarded to the NRO: 16:9 (default) widens the gameplay world; 4:3 keeps the original layout')
    parser.add_argument('--internal-res', choices=['auto', 'half', '1x', '2x'], default='auto', help='Forwarded to the NRO: main-frame render resolution')
    parser.add_argument('--switch-clocks', choices=['fast', 'default'], help='Forwarded to the NRO: request fast CPU/GPU/EMC clocks while focused')
    parser.add_argument('--iop-worker', action='store_true', help='Forwarded to the NRO: run the IOP worker thread pinned to core 2')
    parser.add_argument('--resource-cache', choices=['off', 'file', 'prefetch'], help='Forwarded to the NRO: disc-read RAM cache mode')
    parser.add_argument('--no-iop-worker', action='store_true', help='Forwarded to the NRO: keep the IOP inline on the producer thread')
    parser.add_argument('--reference-particles', action='store_true', help='Forwarded to the NRO: select the reference particle path (setter pending in src/native_particles.h)')
    parser.add_argument('--render-image', choices=['off', 'rendermat', 'image'], help='Forwarded to the NRO: decoupled-render stage 0 (rendermat) or stage 1 (image; needs a NIGHT100_DIRTY_LINES=ON NRO)')
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not 0 <= args.seconds <= 86400:
        parser.error('--seconds must be finite and between 0 and 86400')
    if args.replay_session is not None and not 0 < args.replay_session <= 18446744073709551615:
        parser.error('--replay-session must be a positive uint64 recording number')
    try:
        destination = remote_path(args.root, 'night100.nro')
    except ValueError as error:
        parser.error(str(error))
    binary = (args.binary or args.package / 'night100.nro').resolve()
    if not args.assets_only:
        with binary.open('rb') as stream:
            stream.seek(0x10)
            if stream.read(4) != b'NRO0':
                raise ValueError('Expected an actual libnx NRO executable')
    client = Client(args.host)
    print('Switch:', json.dumps(client.status(), sort_keys=True), flush=True)
    report = {'assets': None, 'nro_uploaded': False, 'nro_bytes': 0}
    if args.assets != 'none':
        report['assets'] = deploy_assets(client, args.package, args.root, args.assets)
    if args.assets_only:
        print(json.dumps(report, indent=2));return 0
    if not args.launch:
        report['nro_uploaded'] = upload_verified(client, binary, destination)
        report['nro_bytes'] = binary.stat().st_size if report['nro_uploaded'] else 0
        print(json.dumps(report, indent=2));return 0
    output = args.output or Path('logs/switch-runs') / str(time.time_ns())
    output.mkdir(parents=True, exist_ok=False)
    run_id = f'host-{time.time_ns()}'
    command_args = ['--root', args.root, '--log-id', run_id, '--seconds', str(args.seconds)]
    if args.skip_videos:command_args.append('--skip-videos')
    if args.gpu_self_test:command_args.append('--gpu-self-test')
    if args.cpu_profile:command_args.append('--cpu-profile')
    if args.native_cpu_profile:command_args.append('--native-cpu-profile')
    if args.stall_watchdog:command_args.append('--stall-watchdog')
    if args.cpu_kernel_benchmark:command_args.append('--cpu-kernel-benchmark')
    if args.replay_session is not None:command_args.extend(['--replay-session', str(args.replay_session)])
    if args.switch_clocks:command_args.extend(['--switch-clocks', args.switch_clocks])
    command_args.extend(['--aspect', args.aspect])
    command_args.extend(['--internal-res', args.internal_res])
    if args.iop_worker:command_args.append('--iop-worker')
    if args.resource_cache:command_args.extend(['--resource-cache', args.resource_cache])
    if args.no_iop_worker:command_args.append('--no-iop-worker')
    if args.reference_particles:command_args.append('--reference-particles')
    if args.render_image:command_args.extend(['--render-image', args.render_image])
    # hbmenu's NetLoader upload path is relative to sdmc:/switch, unlike HTTP.
    relative = Path(destination).relative_to('/switch').as_posix()
    nxlink = os.environ.get('NXLINK', '/opt/devkitpro/tools/bin/nxlink')
    command = [nxlink, '-a', client.ip(), '-p', relative, str(binary), '--args', shlex.join(command_args)]
    # NetLoader creates the NRO but not its parent directory. An HTTP marker
    # creates a new app directory without sending the executable twice.
    try:
        client.read(args.root)
    except urllib.error.HTTPError as error:
        if error.code != 404:raise
        with tempfile.TemporaryDirectory(prefix='night100-netloader-') as temporary:
            marker=Path(temporary)/'ready';marker.write_text('Night100 NetLoader directory\n')
            client.upload(marker,remote_path(args.root,'.netloader-ready'))
    client.screenshot(output / 'before.jpg')
    client.tap('Y');time.sleep(.5)
    with (output / 'nxlink.log').open('w') as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=180)
    report.update(nro_uploaded=True, nro_bytes=binary.stat().st_size, nro_sha256=digest(binary), command=command)
    deadline = time.monotonic() + (90 if args.gpu_self_test else args.seconds + 90)
    text = '';captured = False;settled = False
    while time.monotonic() < deadline:
        try:
            text = client.read(remote_path(args.root, f'logs/run-{run_id}.log')).decode(errors='replace')
            (output / 'runtime.log').write_text(text)
        except urllib.error.HTTPError as error:
            if error.code != 404:raise
        if 'NIGHT100_SWITCH_FATAL' in text:
            break
        if not captured and 'NIGHT100_SWITCH_FRAME_READY' in text:
            client.screenshot(output / 'running.jpg');captured = True
        # The first completed frame can be the legal screen. Preserve a later
        # view from the periodic summary while the application is still live.
        if captured and not settled and '[stage] metrics ' in text and 'NIGHT100_SWITCH_STOP' not in text:
            client.screenshot(output / 'settled.jpg');settled = True
        if 'NIGHT100_SWITCH_GPU_TEST_OK' in text or 'NIGHT100_SWITCH_STOP' in text:
            break
        if args.seconds == 0 and not args.gpu_self_test and settled:
            break  # Interactive launch: retain the live game after diagnostics.
        time.sleep(5)
    report['passed'] = 'NIGHT100_SWITCH_FATAL' not in text and (
        'NIGHT100_SWITCH_GPU_TEST_OK' in text if args.gpu_self_test else
        'NIGHT100_SWITCH_FRAME_READY' in text and ('NIGHT100_SWITCH_STOP' in text or args.seconds == 0 and settled))
    report['status'] = ('running' if report['passed'] and args.seconds == 0 and not args.gpu_self_test and 'NIGHT100_SWITCH_STOP' not in text
                        else 'passed' if report['passed'] else 'failed')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2));print('Evidence:', output)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
