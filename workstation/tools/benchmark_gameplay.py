"""Compare normal-speed gameplay from one checkpoint; leave evidence per run."""
from pathlib import Path
import argparse, datetime, json, os, shutil, subprocess, time

p = argparse.ArgumentParser()
p.add_argument('--fast-module', action='store_true')
p.add_argument('--skip-cache', action='store_true')
p.add_argument('--native-resolution', action='store_true')
p.add_argument('--callback-audit', action='store_true')
p.add_argument('--user-profile', type=Path)
p.add_argument('--profile', action='store_true')
p.add_argument('--fast-os-interrupts', action='store_true')
p.add_argument('--cache-affinity', action='store_true')
p.add_argument('--empty-mod-stubs', action='store_true')
p.add_argument('--sample-cpu', action='store_true')
p.add_argument('--walk', action='store_true', help='Alternate forward/back input every five seconds')
p.add_argument('--module', type=Path)
p.add_argument('--checkpoint', type=Path)
p.add_argument('--runtime', type=Path)
p.add_argument('--fallback-ranges')
p.add_argument('--seconds', type=int, default=30)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
w = root / 'workstation'
log = w / 'logs' / ('gameplay-perf-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
auto = log / 'automation'
cmd = auto / 'commands'
cmd.mkdir(parents=True)
user = log / 'user'
shutil.copytree(a.user_profile.resolve() if a.user_profile else w / 'run-user', user)
if a.native_resolution:
    f = user / 'Config/GFX.ini'
    f.write_text(f.read_text().replace('InternalResolution = 3', 'InternalResolution = 1'))
    f = user / 'config.ini'
    f.write_text(f.read_text().replace('resolution=1920x1080', 'resolution=640x528'))
module = (w / 'build/game-module-fast/gRDHE78_recomp.dll' if a.fast_module
          else root / 'dist/flagship/gRDHE78_recomp.dll')
if a.module:
    module = a.module.resolve()
env = {k:v for k,v in os.environ.items() if not k.startswith(('STATICRECOMP_', 'BIGWILLY_')) and k != 'MODERNGEKKO_CACHE_AFFINITY'}
env.update(STATICRECOMP_FORCE_NUNCHUK='1', STATICRECOMP_FAST_STATS='1',
           STATICRECOMP_AUTOMATION_HARD_OVERRIDE='1')
if a.skip_cache:
    env['BIGWILLY_SKIP_NONCODE_CACHE'] = '1'
if a.callback_audit:
    env['STATICRECOMP_CALLBACK_AUDIT'] = str(log / 'callback-edges.csv')
if a.fast_os_interrupts:
    env['BIGWILLY_FAST_OS_INTERRUPTS'] = '1'
if a.cache_affinity:
    env['MODERNGEKKO_CACHE_AFFINITY'] = '1'
if a.empty_mod_stubs:
    env['STATICRECOMP_SKIP_EMPTY_MOD_CALLS'] = '1'
if a.fallback_ranges:
    env['STATICRECOMP_FALLBACK_RANGES'] = a.fallback_ranges
if a.fast_module:
    env.update(STATICRECOMP_SKIP_EMPTY_MOD_CALLS='1', STATICRECOMP_FAST_NO_HOOK_DISPATCH='1')
if a.profile:
    env.update(STATICRECOMP_DISPATCH_SAMPLES='1', STATICRECOMP_HOOK_SAMPLES='1')
args = [str(a.runtime.resolve() if a.runtime else root / 'dist/flagship/dahbwu.exe'), '--game', str(w / 'game'),
        '--module', str(module), '--user-dir', str(user), '--automation-dir', str(auto),
        '--graphics', 'D3D', '--no-mods', '--background-input', '--load-state',
        str(a.checkpoint.resolve() if a.checkpoint else w / 'interactive-automation/performance-baseline.sav')]
(log / 'configuration.json').write_text(json.dumps({'args':args, 'options':vars(a)}, indent=2, default=str))
(w / 'logs/current-gameplay-perf.txt').write_text(str(log))
print(log, flush=True)
def command(name, body):
    f = cmd / (name + '.tmp')
    f.write_text(body)
    f.replace(cmd / (name + '.txt'))
def status():
    try:
        return dict(s.split('=',1) for s in (auto / 'status.txt').read_text().splitlines() if '=' in s)
    except (FileNotFoundError, PermissionError):
        return {}
with (log / 'stdout.txt').open('w') as so, (log / 'stderr.txt').open('w') as se:
    launched=time.monotonic()
    proc = subprocess.Popen(args, cwd=root / 'dist/flagship', env=env,
        stdout=so, stderr=se, creationflags=subprocess.CREATE_NO_WINDOW)
    (log / 'pid.txt').write_text(str(proc.pid))
    try:
        deadline = time.monotonic()+180
        while status().get('state') != 'running' and proc.poll() is None and time.monotonic()<deadline:
            time.sleep(.5)
        if status().get('state') != 'running':
            raise RuntimeError('startup failed')
        running_seconds=time.monotonic()-launched
        command('001-neutral', 'command=pad\nport=0\nwii_ir_x=0\nwii_ir_y=0\n')
        deadline = time.monotonic()+120
        render_status = status()
        while int(render_status.get('present_count',0))<30 and proc.poll() is None and time.monotonic()<deadline:
            time.sleep(.5)
            render_status = status()
        if int(render_status.get('present_count',0))<30:
            raise RuntimeError('Checkpoint did not resume rendering within 120 seconds')
        (log/'startup.json').write_text(json.dumps({'running_seconds':running_seconds,
            'checkpoint_rendering_seconds':time.monotonic()-launched},indent=2))
        if a.sample_cpu:
            report = subprocess.check_output([str(w / 'build/sample_game_cpu.exe'), str(proc.pid)], text=True, timeout=45)
            (log / 'cpu-samples.txt').write_text(report)
        time.sleep(10)
        command('002-before', 'command=screenshot\npath=before.png\n')
        samples=[]
        start=time.monotonic()
        movement_phase=-1
        while time.monotonic()-start<a.seconds and proc.poll() is None:
            phase=int((time.monotonic()-start)//5)
            if a.walk and phase!=movement_phase:
                movement_phase=phase
                direction=.6 if phase%2==0 else -.6
                command(f'100-walk-{phase:03}', f'command=pad\nport=0\nwii_ir_x=0\nwii_ir_y=0\nnunchuk_y={direction}\n')
                command(f'101-view-{phase:03}', f'command=screenshot\npath=walk-{phase:03}.png\n')
            samples.append({'t':time.monotonic()-start, **status()})
            time.sleep(1)
        (log / 'samples.json').write_text(json.dumps(samples, indent=2))
        command('200-neutral', 'command=pad\nport=0\nwii_ir_x=0\nwii_ir_y=0\n')
        command('003-after', 'command=screenshot\npath=after.png\n')
        time.sleep(1)
        if len(samples)>1:
            fps=[float(s.get('fps',0)) for s in samples]
            summary={'mean_fps':sum(fps)/len(fps), 'min_fps':min(fps),
                'counter_fps':(int(samples[-1]['frame_count'])-int(samples[0]['frame_count']))/(samples[-1]['t']-samples[0]['t']),
                'elapsed':samples[-1]['t']-samples[0]['t'], 'samples':len(samples)}
            (log / 'summary.json').write_text(json.dumps(summary,indent=2))
            print(summary, flush=True)
    finally:
        command('999-stop','command=stop\n')
        try:
            proc.wait(20)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(10)
        (log / 'exit.txt').write_text(str(proc.returncode))
