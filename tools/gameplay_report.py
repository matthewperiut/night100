#!/usr/bin/env python3
"""Summarize completed game frames; never substitute window redraws for FPS."""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=Path)
p.add_argument('--cpu-scale', type=float, default=1)
p.add_argument('--gpu-scale', type=float, default=39)
p.add_argument('--duration', type=float, required=True)
p.add_argument('--exit-code', type=int, default=0)
a = p.parse_args()
def records(name):
    path=a.directory/name
    if not path.exists():
        return []
    with path.open() as f:
        return list(csv.DictReader(f))
rows = records('frames.csv')
events = records('events.csv')
manifest_path=a.directory/'manifest.json'
manifest=json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
def percentile(values, q):
    return sorted(values)[min(len(values)-1, math.ceil(len(values)*q)-1)] if values else None
def summarize(group):
    if not group:
        return {'frames': 0, 'fps': None}
    wall = [float(r['wall_ms']) for r in group]
    cpu = [float(r['game_thread_cpu_ms']) for r in group]
    process = [float(r['process_cpu_ms']) for r in group]
    gpu = []
    for before, after in zip(group, group[1:]):
        if int(after['frame']) != int(before['frame'])+1:
            continue
        if int(after.get('gpu_samples', 0)) > int(before.get('gpu_samples', 0)):
            gpu.append((int(after['gpu_ns'])-int(before['gpu_ns']))/1e6)
    gpu_mean = statistics.mean(gpu) if gpu else None
    presentation = None
    presentation_keys = ('gpu_completed_frames', 'cpu_completed_fallbacks', 'retained_completed_frames')
    if all(all(r.get(key) not in (None, '') for key in presentation_keys) for r in group):
        presentation = {key: sum(int(after[key])-int(before[key])
                                for before, after in zip(group, group[1:])
                                if int(after['frame']) == int(before['frame'])+1)
                        for key in presentation_keys}
    # Development estimates from the supplied 10x per-core slowdown. Keep
    # serial and aggregate limits separate: spare cores do not shorten serial
    # work. Neither estimate substitutes for a target-device measurement.
    serial_budget = (1000 / 60) / 10
    aggregate_budget = serial_budget * 4
    cpu_budget = {
        'serial_host_budget_ms': serial_budget,
        'four_core_host_budget_ms': aggregate_budget,
        'serial_mean_fraction': statistics.mean(cpu) / serial_budget,
        'serial_p95_fraction': percentile(cpu, .95) / serial_budget,
        'aggregate_mean_fraction': statistics.mean(process) / aggregate_budget,
        'aggregate_p95_fraction': percentile(process, .95) / aggregate_budget,
        'serial_p95_fits': percentile(cpu, .95) <= serial_budget,
        'aggregate_p95_fits': percentile(process, .95) <= aggregate_budget,
    }
    return {'frames': len(group), 'fps': 1000/statistics.mean(wall),
            'wall_ms_p50': percentile(wall, .5), 'wall_ms_p95': percentile(wall, .95),
            'wall_ms_p99': percentile(wall, .99), 'wall_ms_max': max(wall),
            'over_16_667_ms': sum(t > 1000/60 for t in wall),
            'over_20_ms': sum(t > 20 for t in wall),
            'game_thread_cpu_ms_mean': statistics.mean(cpu), 'game_thread_cpu_ms_p95': percentile(cpu, .95),
            'process_cpu_ms_mean': statistics.mean(process),
            'executor_cpu_fraction_of_wall': sum(cpu)/sum(wall),
            'serial_cpu_ms_estimate_at_10x': statistics.mean(cpu)*10,
            'four_core_cpu_ms_estimate_at_10x': statistics.mean(process)*10/4,
            'small_cpu_budget_estimate': cpu_budget,
            'completed_presentation': presentation,
            'native_render_textures': {key: sum(int(after[key])-int(before[key])
                for before, after in zip(group, group[1:])
                if int(after['frame']) == int(before['frame'])+1)
                for key in ('native_render_texture_copies', 'native_render_texture_fallbacks')
                if all(r.get(key) not in (None, '') for r in group)} or None,
            'gpu_elapsed_ms_mean': gpu_mean,
            'gpu_elapsed_ms_times_ratio': gpu_mean*a.gpu_scale if gpu_mean is not None else None}

stop = next((float(e['seconds']) for e in reversed(events) if e['event']=='stop'), a.duration)
scene_events = [e for e in events if e['event']=='scene-init' and int(e['value'])==int.from_bytes(b'h001', 'big')]
load_start = float(scene_events[0]['seconds']) if scene_events else None
# DWARF enums: mode 11=Game, ostrich 2=InScene. Exclude stale pre-load mode changes.
in_scene = [r for r in rows if load_start is not None and float(r['seconds'])>load_start
            and int(r['mode'])==11 and int(r['ostrich'])==2]
gameplay = [r for r in in_scene if int(r.get('cutscene',0))==0 and int(r.get('player_control',1))!=0]
last_frame = float(rows[-1]['seconds']) if rows else 0
report = {'target': {'platform': 'Nintendo Switch (future NRO and separate GPU backend)',
                     'cpu_cores': 4, 'cpu': 'Cortex-A57', 'per_core_slowdown_assumption': 10,
                     'gpu': '256-core Maxwell', 'gpu_clock_mhz': 307, 'gpu_gflops': 156,
                     'fps': 60, 'frame_budget_ms': 1000/60},
          'proxy': {'executor_cpu_scale': a.cpu_scale, 'gpu_timing_multiplier': a.gpu_scale,
                    'gpu_hardware_throttled': False,
                    'limitations': 'CPU duty cycling is not A57 emulation or a four-core affinity limit. GL elapsed times include pipeline/driver stalls and submission gaps; GFLOPS scaling does not model bandwidth, fill rate or Maxwell. Window GPU work is excluded.'},
          'duration_s': stop, 'runtime_exit_code': a.exit_code,
          'tail_without_completed_frame_s': max(0, stop-last_frame),
          'h001_scene_init_s': load_start,
          'h001_load_to_first_in_scene_frame_s': float(in_scene[0]['seconds'])-load_start if in_scene else None,
          'h001_load_to_first_playable_frame_s': float(gameplay[0]['seconds'])-load_start if gameplay else None,
          'title_settled': summarize([r for r in rows if int(r['mode'])==2 and 1<float(r['seconds']) and (load_start is None or float(r['seconds'])<load_start)]),
          'loading': summarize([r for r in rows if load_start is not None and float(r['seconds'])>load_start and int(r['ostrich'])==0]),
          'cutscene': summarize([r for r in rows if int(r.get('cutscene',0))!=0 and int(r['ostrich'])==2]),
          'gameplay': summarize(gameplay)}
loop_entry=next((float(e['seconds']) for e in events if e['event']=='game-loop-entry' and load_start is not None and float(e['seconds'])>=load_start),None)
report['h001_scene_init_to_game_loop_entry_s']=loop_entry-load_start if loop_entry is not None else None
if load_start is not None:
    load_end=float(in_scene[0]['seconds']) if in_scene else stop
    load_seconds=load_end-load_start
    report['loading']['interval_s']=load_seconds
    report['loading']['fps_over_entire_load']=report['loading']['frames']/load_seconds if load_seconds>0 else None
presentation = report['gameplay'].get('completed_presentation')
report['gameplay_60fps_timing_pass'] = bool(len(gameplay)>=600 and report['tail_without_completed_frame_s']<1 and report['gameplay']['fps']>=59.9 and report['gameplay']['wall_ms_p99']<=20 and not a.exit_code
                                          and not (presentation and presentation['retained_completed_frames']))
report['target_hardware_60fps_verified'] = False
if manifest.get('debug_manor_warp_unlock'):
    portal=next((float(e['seconds']) for e in events if e['event']=='debug-manor-original-portal-request'),None)
    interior=[r for r in gameplay if int(r.get('scene_id',0))==int.from_bytes(b'I001','big')
              and portal is not None and float(r['seconds'])>portal]
    report['manor_transition']={
        'debug_warp_and_original_gate_unlock': True,
        'original_portal_request_s': portal,
        'portal_to_first_controllable_interior_frame_s': float(interior[0]['seconds'])-portal if interior else None,
        'interior_gameplay': summarize(interior),
        'passed': bool(len(interior)>=120 and report['tail_without_completed_frame_s']<1 and not a.exit_code)}
report['benchmark_pass']=report['manor_transition']['passed'] if 'manor_transition' in report else report['gameplay_60fps_timing_pass']
if manifest.get('debug_snack_collision'):
    warp=next((e for e in events if e['event']=='debug-snack-warp'),None)
    pickup=next((e for e in events if warp and e['event']=='snack-count'
                 and int(e['value'])>0 and float(e['seconds'])>float(warp['seconds'])),None)
    pickup_frame=int(pickup['frame']) if pickup else -1000
    report['snack_collision']={
        'debug_warp_then_original_walking_collision': True,
        'collected_snacks': max((int(r.get('snacks',0)) for r in gameplay),default=0),
        'first_pickup_s': float(pickup['seconds']) if pickup else None,
        'pickup_window': summarize([r for r in gameplay if pickup_frame-3<=int(r['frame'])<=pickup_frame+12]),
        'passed': bool(pickup and any(int(r.get('snacks',0))>0 for r in gameplay) and not a.exit_code)}
    report['benchmark_pass']=report['gameplay_60fps_timing_pass'] and report['snack_collision']['passed']
if manifest.get('debug_bats_key_and_hallway'):
    names=['debug-bats-key-warp','debug-bats-key-collected','debug-bats-trigger-enabled',
           'debug-bats-hall-start','debug-bats-walk','debug-bats-triggered',
           'debug-bats-wall-translucent','debug-bats-steer-right','debug-bats-stop-walking']
    checkpoints={name:next((e for e in events if e['event']==name),None) for name in names}
    missing=[name for name in names if checkpoints[name] is None]
    ordered=not missing and all(float(checkpoints[before]['seconds'])<=float(checkpoints[after]['seconds'])
                              and int(checkpoints[before]['frame'])<=int(checkpoints[after]['frame'])
                              for before,after in zip(names,names[1:]))
    def checkpoint_time(name):
        event=checkpoints.get(name)
        return float(event['seconds']) if event else None
    bat_time=checkpoint_time('debug-bats-triggered')
    wall_time=checkpoint_time('debug-bats-wall-translucent')
    stop_time=checkpoint_time('debug-bats-stop-walking')
    key_time=checkpoint_time('debug-bats-key-collected')
    alpha_event=next((e for e in events if e['event']=='debug-bats-wall-alpha'
                      and wall_time is not None and float(e['seconds'])>=wall_time),None)
    alpha=int(alpha_event['value']) if alpha_event else None
    route=[r for r in gameplay if int(r.get('scene_id',0))==int.from_bytes(b'I001','big')
           and bat_time is not None and float(r['seconds'])>=bat_time]
    stop_event=checkpoints['debug-bats-stop-walking']
    stopped=[r for r in route if stop_event is not None and int(r['frame'])>=int(stop_event['frame'])]
    positions=[(float(r['player_x']),float(r['player_y']),float(r['player_z'])) for r in stopped
               if all(r.get(axis) not in (None,'') for axis in ('player_x','player_y','player_z'))]
    furthest_z=min((p[2] for p in positions),default=None)
    reached=bool(positions and positions[0][2] < -18)
    reasons=[]
    if missing:reasons.append('Missing original route checkpoints: '+', '.join(missing))
    elif not ordered:reasons.append('Key, bat, hallway and steering checkpoints are out of order')
    if alpha is None or not 0<=alpha<=128:reasons.append('Original hallway wall fade was not observed')
    if not reached:reasons.append('Right steering did not reach beyond hallway Z=-18 after walking stopped')
    if not report.get('manor_transition',{}).get('passed'):reasons.append('Original manor transition was not verified')
    if report['tail_without_completed_frame_s']>=1:reasons.append('Completed gameplay frames stopped before the run ended')
    if a.exit_code:reasons.append('Runtime exited with an error')
    report['bats_hallway']={
        'debug_key_collision_then_original_bat_and_camera_triggers':True,
        'key_collected_s':key_time,'bats_triggered_s':bat_time,
        'key_to_bats_s':bat_time-key_time if bat_time is not None and key_time is not None else None,
        'hallway_translucent_s':wall_time,
        'bats_to_hallway_s':wall_time-bat_time if wall_time is not None and bat_time is not None else None,
        'wall_alpha':alpha,'wall_fade_s':float(alpha_event['seconds']) if alpha_event else None,
        'steer_right_s':checkpoint_time('debug-bats-steer-right'),'walk_stopped_s':stop_time,
        'ordered_original_event_chain':bool(ordered),'hallway_route_completed':reached,
        'furthest_hallway_z':furthest_z,'stopped_position':positions[0] if positions else None,
        'final_position':positions[-1] if positions else None,
        'gameplay_after_bats':summarize(route),'failure_reasons':reasons,'passed':not reasons}
    report['benchmark_pass']=report['bats_hallway']['passed']
(a.directory/'report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['benchmark_pass'] else 2)
