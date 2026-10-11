"""Exercise movie-skip and CPU pauses without taking desktop input control."""
from pathlib import Path
import argparse, datetime, json, os, shutil, subprocess, time

p = argparse.ArgumentParser()
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--module', type=Path)
p.add_argument('--checkpoint', type=Path)
p.add_argument('--seconds', type=int, default=70)
p.add_argument('--startup-timeout', type=int, default=600)
p.add_argument('--scenarios', action='store_true', help='Exercise Crypto, mech and saucer checkpoints in one process')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
w = root / 'workstation'
log = w/'logs'/('callback-runtime-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
auto = log/'automation'
commands = auto/'commands'
commands.mkdir(parents=True)
shutil.copytree(w/'run-user', log/'user')
env = {k:v for k,v in os.environ.items() if not k.startswith(('STATICRECOMP_', 'BIGWILLY_'))}
env.update(STATICRECOMP_FORCE_NUNCHUK='1', STATICRECOMP_AUTOMATION_HARD_OVERRIDE='1',
           STATICRECOMP_CALLBACK_AUDIT=str(log/'callback-edges.csv'))
args = [str(a.runtime.resolve()), '--game', str(w/'game'), '--module',
        str(a.module.resolve() if a.module else root/'dist/flagship/gRDHE78_recomp.dll'), '--user-dir', str(log/'user'),
        '--automation-dir', str(auto), '--graphics', 'D3D', '--no-mods', '--background-input']
if a.checkpoint:
    args += ['--load-state', str(a.checkpoint.resolve())]
(log/'configuration.json').write_text(json.dumps(args, indent=2))
print(log, flush=True)
(w/'logs/current-callback-runtime.txt').write_text(str(log))
sequence = 0
def send(body):
    global sequence
    sequence += 1
    tmp = commands/f'{sequence:05}.tmp'
    tmp.write_text(body)
    tmp.replace(tmp.with_suffix('.txt'))
    return tmp.with_suffix('.txt').name
def status():
    try:
        return dict(x.split('=',1) for x in (auto/'status.txt').read_text().splitlines() if '=' in x)
    except (FileNotFoundError, PermissionError):
        return {}
with (log/'stdout.txt').open('w') as out, (log/'stderr.txt').open('w') as err:
    proc = subprocess.Popen(args, cwd=root/'dist/flagship', env=env, stdout=out, stderr=err,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    (log/'pid.txt').write_text(str(proc.pid))
    try:
        deadline = time.monotonic()+a.startup_timeout
        while status().get('state') != 'running' and proc.poll() is None and time.monotonic()<deadline:
            time.sleep(.5)
        if status().get('state') != 'running':
            raise RuntimeError('Runtime failed to reach running state')
        start = time.monotonic()
        samples = []
        while time.monotonic()-start < a.seconds and proc.poll() is None:
            t = time.monotonic()-start
            samples.append({'t':t, **status()})
            # Both operations travel through the runtime's game-only command queue.
            # ReadMemory uses CPUThreadGuard, exercising Run() exit/re-entry.
            send('command=skip_movie\n')
            if int(status().get('present_count',0)) >= 30:
                send('command=read_memory\naddress=0x80000000\nsize=64\npath=probe.bin\n')
            if len(samples)%5 == 0 and int(status().get('present_count',0)) >= 30:
                send(f'command=screenshot\npath=frame-{len(samples):03}.png\n')
            time.sleep(1)
        (log/'samples.json').write_text(json.dumps(samples, indent=2))
        if proc.poll() is not None:
            raise RuntimeError(f'Runtime exited during callback test: {proc.returncode}')
        if not samples or int(samples[-1].get('present_count',0)) < 30:
            raise RuntimeError('Callback test did not produce enough rendered frames')
        if a.scenarios:
            for name,checkpoint in [('crypto','performance-baseline.sav'),
                                    ('mech','bigwilly-performance.sav'),
                                    ('saucer','midwest-saucer.sav')]:
                command=send(f'command=load_state\npath={w/"interactive-automation"/checkpoint}\n')
                deadline=time.monotonic()+60
                while status().get('last_command') != command and proc.poll() is None and time.monotonic()<deadline:
                    time.sleep(.1)
                if status().get('last_command') != command:
                    raise RuntimeError(f'{name}: checkpoint command did not complete')
                time.sleep(5)
                send(f'command=screenshot\npath={name}-before.png\n')
                phase_samples=[]
                for second in range(30):
                    if proc.poll() is not None:
                        raise RuntimeError(f'{name}: runtime exited with {proc.returncode}')
                    if second%5 == 0:
                        direction=.6 if (second//5)%2 == 0 else -.6
                        send(f'command=pad\nport=0\nnunchuk_y={direction}\nwii_ir_x=0\nwii_ir_y=0\n')
                    if name=='crypto' and second==5:
                        send('command=giveallweapons\n')
                        send('command=skip_movie\n')
                    if name=='crypto' and second==20:
                        send('command=enter_bigwilly\n')
                    send('command=read_memory\naddress=0x80000000\nsize=64\npath=probe.bin\n')
                    phase_samples.append({'t':second,**status()})
                    time.sleep(1)
                send('command=pad\nport=0\nnunchuk_y=0\nwii_ir_x=0\nwii_ir_y=0\n')
                send(f'command=screenshot\npath={name}-after.png\n')
                (log/f'{name}-samples.json').write_text(json.dumps(phase_samples,indent=2))
                frames=int(phase_samples[-1].get('frame_count',0))-int(phase_samples[0].get('frame_count',0))
                if frames<30:
                    raise RuntimeError(f'{name}: rendering stalled during scenario')
                print(f'{name}: {frames} frames during 30-second callback/pause exercise',flush=True)
                time.sleep(1)
    finally:
        send('command=stop\n')
        try:
            proc.wait(20)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(10)
        (log/'exit.txt').write_text(str(proc.returncode))
print('Callback runtime test finished; inspect skip-completion logs and captures.', flush=True)
