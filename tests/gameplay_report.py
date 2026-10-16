"""A high window/camera count must never hide stalled loading or missing samples."""
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    folder=Path(temp)
    fields='frame,seconds,wall_ms,game_thread_cpu_ms,process_cpu_ms,mode,state,ostrich,cutscene,player_control'.split(',')
    def run(rows, events):
        with (folder/'frames.csv').open('w') as f:
            writer=csv.writer(f);writer.writerow(fields)
            writer.writerows(row+[0,1] if len(row)==8 else row for row in rows)
        with (folder/'events.csv').open('w') as f:
            writer=csv.writer(f);writer.writerow(['seconds','frame','event','value']);writer.writerows(events)
        result=subprocess.run([sys.executable,str(root/'tools/gameplay_report.py'),str(folder),'--duration','20'],capture_output=True,text=True)
        return result.returncode,json.loads((folder/'report.json').read_text())
    scene=int.from_bytes(b'h001','big')
    events=[[1,60,'scene-init',scene],[20,1200,'stop',0]]
    # Plenty of camera presents before a permanent loading stall.
    rows=[[i,i/60,1000/60,1,2,2,0,2] for i in range(1,601)]
    code,report=run(rows,events)
    assert code==2 and report['gameplay']['frames']==0
    assert report['tail_without_completed_frame_s']==10
    # Gameplay reaches 60 on this host; this is still not target validation.
    rows=[[i,i/60,1000/60,1,2,11,0,2] for i in range(61,1201)]
    code,report=run(rows,events)
    assert code==0 and report['gameplay_60fps_timing_pass']
    assert not report['target_hardware_60fps_verified']
    budget=report['gameplay']['small_cpu_budget_estimate']
    assert budget['serial_p95_fits'] and budget['aggregate_p95_fits']
    assert abs(budget['serial_mean_fraction']-.6)<1e-9
    assert abs(budget['aggregate_mean_fraction']-.3)<1e-9
    # Host 60 FPS can still exceed the weak CPU's serial budget even when
    # aggregate work would fit four cores. Slow frames cannot hide in a mean.
    expensive=[row[:3]+[2,3]+row[5:] for row in rows]
    code,report=run(expensive,events)
    assert code==0 and report['gameplay_60fps_timing_pass']
    budget=report['gameplay']['small_cpu_budget_estimate']
    assert not budget['serial_p95_fits'] and budget['aggregate_p95_fits']
    bursty=[row[:3]+([3,8] if row[0]%10==0 else [1,2])+row[5:] for row in rows]
    code,report=run(bursty,events)
    budget=report['gameplay']['small_cpu_budget_estimate']
    assert budget['serial_mean_fraction']<1 and budget['aggregate_mean_fraction']<1
    assert not budget['serial_p95_fits'] and not budget['aggregate_p95_fits']
    # Submitted game frames cannot hide a bounded GPU producer reusing an old
    # completed image. Preserve backward compatibility with older telemetry.
    assert report['gameplay']['completed_presentation'] is None
    fields.extend(['gpu_completed_frames','cpu_completed_fallbacks','retained_completed_frames'])
    presented=[row+[0,1,row[0],0,0] for row in rows]
    code,report=run(presented,events)
    assert code==0 and report['gameplay']['completed_presentation']['gpu_completed_frames']==len(rows)-1
    retained=[row+[0,1,row[0]-int(row[0]>=600),0,int(row[0]>=600)] for row in rows]
    code,report=run(retained,events)
    assert code==2 and not report['gameplay_60fps_timing_pass']
    assert report['gameplay']['completed_presentation']['retained_completed_frames']==1
    del fields[-3:]
    fields.extend(['native_render_texture_copies','native_render_texture_fallbacks'])
    imported=[row+[0,1,row[0],0] for row in rows]
    code,report=run(imported,events)
    assert report['gameplay']['native_render_textures']=={
        'native_render_texture_copies':len(rows)-1,'native_render_texture_fallbacks':0}
    del fields[-2:]
    # A playing cinematic has finished loading, but is not controllable gameplay.
    cinematic=[row+[1,0] for row in rows]
    code,report=run(cinematic,events)
    assert code==2 and report['gameplay']['frames']==0
    assert report['h001_load_to_first_in_scene_frame_s']<.02
    assert report['h001_load_to_first_playable_frame_s'] is None
    assert report['loading']['interval_s']<.02
    # A run ending on a stale gameplay frame must fail, even after 600 frames.
    code,report=run(rows[:650],events)
    assert code==2 and report['tail_without_completed_frame_s']>1
    # Continuing to present H001 after a portal request is not a successful
    # transition, even when its old frames satisfy the timing criterion.
    (folder/'manifest.json').write_text(json.dumps({'debug_manor_warp_unlock':True}))
    portal_events=events+[[5,300,'debug-manor-original-portal-request',0]]
    code,report=run(rows,portal_events)
    assert code==2 and not report['manor_transition']['passed']
    assert report['manor_transition']['interior_gameplay']['frames']==0
    # Require actual controllable I001 frames after that original request.
    fields.append('scene_id')
    interior_id=int.from_bytes(b'I001','big')
    interior_rows=[row+[0,1,interior_id] for row in rows]
    code,report=run(interior_rows,portal_events)
    assert code==0 and report['manor_transition']['passed']
    assert report['manor_transition']['portal_to_first_controllable_interior_frame_s']>0
    fields.pop();(folder/'manifest.json').unlink()
    (folder/'manifest.json').write_text(json.dumps({'debug_snack_collision':True}))
    snack_events=events+[[5,300,'debug-snack-warp',0],[6,360,'snack-count',1]]
    code,report=run(rows,snack_events)
    assert code==2 and not report['snack_collision']['passed']
    fields.append('snacks')
    code,report=run([row+[0,1,int(row[0]>=360)] for row in rows],snack_events)
    assert code==0 and report['snack_collision']['passed']
    assert report['snack_collision']['collected_snacks']==1
    assert report['snack_collision']['pickup_window']['frames']==16
    fields.pop();(folder/'manifest.json').unlink()
    # The bat scenario must prove its key collision and original event chain,
    # then continue right after the camera turns instead of stopping at a wall.
    (folder/'manifest.json').write_text(json.dumps({'debug_manor_warp_unlock':True,
                                                   'debug_bats_key_and_hallway':True}))
    fields.extend(['scene_id','player_x','player_y','player_z'])
    bat_events=portal_events+[
        [6,360,'debug-bats-key-warp',1],[6.1,366,'debug-bats-key-collected',68],
        [6.1,366,'debug-bats-trigger-enabled',2],[7,420,'debug-bats-hall-start',0],
        [8,480,'debug-bats-walk',0],[9,540,'debug-bats-triggered',2],
        [10,600,'debug-bats-wall-translucent',3],[10.4,624,'debug-bats-wall-alpha',126],
        [10.5,630,'debug-bats-steer-right',0],[12,720,'debug-bats-stop-walking',0]]
    bat_rows=[row+[0,1,interior_id,1.14,0,-3-min(16,max(0,row[1]-8)*4)] for row in rows]
    code,report=run(bat_rows,bat_events)
    assert code==0 and report['bats_hallway']['passed']
    assert report['bats_hallway']['ordered_original_event_chain']
    assert report['bats_hallway']['furthest_hallway_z']==-19
    assert report['bats_hallway']['wall_alpha']==126
    code,report=run(bat_rows,[e for e in bat_events if e[2]!='debug-bats-key-collected'])
    assert code==2 and not report['bats_hallway']['passed']
    assert report['manor_transition']['passed']  # Interior rendering alone is insufficient.
    code,report=run(bat_rows,[e for e in bat_events if e[2]!='debug-bats-steer-right'])
    assert code==2 and not report['bats_hallway']['ordered_original_event_chain']
    wrong_order=[([5.5,330,*e[2:]] if e[2]=='debug-bats-triggered' else e) for e in bat_events]
    code,report=run(bat_rows,wrong_order)
    assert code==2 and not report['bats_hallway']['ordered_original_event_chain']
    code,report=run([row[:-1]+[-13.66] for row in bat_rows],bat_events)
    assert code==2 and not report['bats_hallway']['hallway_route_completed']
    code,report=run(bat_rows[:850],bat_events)
    assert code==2 and not report['bats_hallway']['passed']
    assert report['tail_without_completed_frame_s']>1
    code,report=run(bat_rows,[e for e in bat_events if e[2]!='debug-bats-wall-alpha'])
    assert code==2 and report['bats_hallway']['wall_alpha'] is None
    del fields[-4:];(folder/'manifest.json').unlink()
    # Empty telemetry also fails, without inventing a frame rate.
    code,report=run([],events)
    assert code==2 and report['gameplay']['fps'] is None
    (folder/'frames.csv').unlink();(folder/'events.csv').unlink()
    result=subprocess.run([sys.executable,str(root/'tools/gameplay_report.py'),str(folder),'--duration','20','--exit-code','1'],capture_output=True)
    assert result.returncode==2
    assert json.loads((folder/'report.json').read_text())['runtime_exit_code']==1
print('PASS: loading stalls, host-only throughput, stale gameplay, bat/key/hallway event chain and empty telemetry')
