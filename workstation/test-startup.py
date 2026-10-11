"""Bounded launch of our built runner, with status and graceful-stop evidence."""
from pathlib import Path
import json
import os
import subprocess
import time

root = Path(__file__).resolve().parent.parent
work = root / 'workstation'
mode = os.environ.get('BIGWILLY_TEST_MODE', 'headless')
frame_limit = int(os.environ.get('BIGWILLY_TEST_FRAMES', '180'))
timeout_seconds = int(os.environ.get('BIGWILLY_TEST_TIMEOUT', '180'))
if frame_limit < 1 or timeout_seconds < 1:
    raise ValueError('Frame limit and timeout must be positive')
stamp = time.strftime('%Y%m%d-%H%M%S')
out = work / 'logs' / f'startup-{mode}-{stamp}'
out.mkdir(parents=True)
automation = out / 'automation'
commands = automation / 'commands'
commands.mkdir(parents=True)
package = root / 'dist/flagship'
args = [str(package / 'dahbwu.exe'), '--game', str(work / 'game'),
        '--module', str(package / 'gRDHE78_recomp.dll'), '--user-dir', str(work / 'run-user'),
        '--automation-dir', str(automation), '--no-mods']
if mode == 'headless':
    args += ['--headless', '--graphics', 'Null']
else:
    args += ['--graphics', 'D3D', '--mute']
print(f'Test logs: {out}', flush=True)
started = time.monotonic()
status = {}
reason = 'process exited'
with (out/'stdout.txt').open('w') as stdout, (out/'stderr.txt').open('w') as stderr:
    process = subprocess.Popen(args, cwd=package, stdout=stdout, stderr=stderr,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    while process.poll() is None:
        try:
            status = dict(line.split('=',1) for line in (automation/'status.txt').read_text().splitlines() if '=' in line)
        except (FileNotFoundError, PermissionError):
            pass
        if int(status.get('frame_count', '0')) >= frame_limit:
            reason = f'reached {frame_limit} frames'
            if mode != 'headless':
                (commands/'001-screenshot.txt').write_text('command=screenshot\npath=frame.png\n')
                for _ in range(20):
                    if (automation/'frame.png').exists() or process.poll() is not None:
                        break
                    time.sleep(.5)
            break
        if time.monotonic() - started > timeout_seconds:
            reason = f'{timeout_seconds}-second timeout'
            break
        time.sleep(.5)
    (out/'observed-status.json').write_text(json.dumps(status,indent=2))
    forced = False
    if process.poll() is None:
        (commands/'999-stop.txt').write_text('command=stop\n')
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            forced = True
            process.terminate()
            process.wait(timeout=10)
report = {'mode':mode, 'frame_limit':frame_limit, 'timeout_seconds':timeout_seconds,
          'exit_code':process.returncode, 'reason':reason,
          'forced_stop':forced, 'elapsed_seconds':round(time.monotonic()-started,2),
          'observed_status':status}
(out/'result.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2),flush=True)
