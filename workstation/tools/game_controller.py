"""Furon Dev Console — game controller and developer console server.

Modes:
  serve   Launch a new agent session and serve the dev console.
          python game_controller.py serve [--load checkpoint.sav] [--port 8080]

  attach  Attach the dev console to an already-running interactive session.
          python game_controller.py attach [--automation-dir path] [--port 8080]

  status  Print the current session status JSON.
          python game_controller.py status

  send    Queue a single action against the running session.
          python game_controller.py send --action pad --seconds 2 --nunchuk-y 1
"""
from __future__ import annotations

import argparse
import collections
import configparser
import datetime as dt
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import threading
import time
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'workstation'
ACTIVE = WORK / 'controller-session.json'
FURONLOG = WORK / 'logs' / 'bigwilly.furonlog'
INTERACTIVE_AUTO = WORK / 'interactive-automation'

BUTTONS = {'wii_a', 'wii_b', 'wii_1', 'wii_2', 'wii_plus', 'wii_minus', 'wii_home',
           'wii_dpad_up', 'wii_dpad_down', 'wii_dpad_left', 'wii_dpad_right',
           'nunchuk_c', 'nunchuk_z'}
AXES = {'nunchuk_x', 'nunchuk_y', 'wii_ir_x', 'wii_ir_y'}


# ── helpers ──────────────────────────────────────────────────────────────────

def atomic_text(path, text):
    path = Path(path)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(text, encoding='utf-8')
    tmp.replace(path)


def read_tail(path: Path, n: int) -> list[str]:
    try:
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
        return lines[-n:]
    except OSError:
        return []


def list_checkpoints() -> list[dict]:
    result = []
    seen = set()
    for p in sorted(WORK.rglob('*.sav'), key=lambda x: x.stat().st_mtime if x.exists() else 0, reverse=True):
        # Skip build artifacts and hidden dirs
        parts = p.parts
        if any(s.startswith('.') or s in ('build', '__pycache__') for s in parts):
            continue
        if p.name in seen:
            continue
        seen.add(p.name)
        try:
            result.append({'name': p.name, 'path': str(p), 'mtime': p.stat().st_mtime})
        except OSError:
            pass
        if len(result) >= 50:
            break
    return result


def validate_pad(controls, seconds):
    if not isinstance(controls, dict) or set(controls) - (AXES | BUTTONS):
        raise ValueError('Unknown Wii/Nunchuk control')
    seconds = float(seconds)
    if not math.isfinite(seconds) or not 0.05 <= seconds <= 60:
        raise ValueError('Hold duration must be 0.05..60 wall-clock seconds')
    result = {}
    for key, value in controls.items():
        value = float(value)
        if not math.isfinite(value) or not (-1 if key in AXES else 0) <= value <= 1:
            raise ValueError(f'Invalid value for {key}')
        result[key] = value
    return result, seconds


# ── PadLease ─────────────────────────────────────────────────────────────────

class PadLease:
    def __init__(self):
        self.deadline = None

    def arm(self, seconds, now):
        self.deadline = now + seconds

    def expire(self, now):
        if self.deadline is not None and now >= self.deadline:
            self.deadline = None
            return True
        return False


# ── exclusive profile setup ──────────────────────────────────────────────────

def exclusive_profile(destination):
    shutil.copytree(WORK / 'run-user-cold-world-a-after-1', destination)
    (destination / 'config.ini').write_text(
        '[Video]\nresolution=640x528\nbackend=Vulkan\nfullscreen=false\n'
        'show_fps_in_title=true\n[Input]\ncontroller=\n', encoding='utf-8')
    for filename, prefix in [('WiimoteNew.ini', 'Wiimote'), ('GCPadNew.ini', 'GCPad')]:
        path = destination / 'Config' / filename
        cfg = configparser.ConfigParser(interpolation=None, strict=False)
        cfg.optionxform = str
        if path.exists():
            cfg.read(path)
        for section in cfg.sections():
            for key in list(cfg[section]):
                if '/' in key or key == 'Device':
                    cfg[section][key] = ''
        for port in range(1, 5):
            section = f'{prefix}{port}'
            if not cfg.has_section(section):
                cfg.add_section(section)
            cfg[section]['Device'] = ''
            if prefix == 'Wiimote':
                cfg[section]['Source'] = '1' if port == 1 else '0'
                cfg[section]['Extension'] = 'Nunchuk' if port == 1 else 'None'
                cfg[section]['Options/Sideways Wiimote'] = 'False'
        with path.open('w') as stream:
            cfg.write(stream)
    path = destination / 'Config/Dolphin.ini'
    cfg = configparser.ConfigParser(interpolation=None, strict=False)
    cfg.optionxform = str
    cfg.read(path)
    for section in ('Display', 'Input', 'Core'):
        if not cfg.has_section(section):
            cfg.add_section(section)
    cfg['Display'].update(Fullscreen='False', RenderWindowXPos='60', RenderWindowYPos='60',
                          RenderWindowWidth='1100', RenderWindowHeight='760')
    cfg['Input']['BackgroundInput'] = 'True'
    cfg['Core']['EmulationSpeed'] = '1.0'
    with path.open('w') as stream:
        cfg.write(stream)


# ── Controller (agent-session, owns a process) ───────────────────────────────

class Controller:
    def __init__(self, session, process):
        self.session = session
        self.auto = session / 'automation'
        self.process = process
        self.lock = threading.RLock()
        self.events = collections.deque(maxlen=60)
        self.pending = {}
        self.controls = {}
        self.lease = PadLease()
        self.sequence = 0
        self.stopping = False
        self.note = 'Starting game; waiting for the emulated core.'
        self.last_status = {}
        self.fps_history: collections.deque = collections.deque(maxlen=120)
        self.next_capture = 0
        self.next_sample = 0
        self.image = b''
        self.image_time = None
        self.image_mtime = 0
        self.started = time.time()

    def event(self, kind, text):
        event = {'at': dt.datetime.now().isoformat(timespec='seconds'), 'kind': kind, 'text': text}
        self.events.appendleft(event)
        with (self.session / 'actions.jsonl').open('a', encoding='utf-8') as file:
            file.write(json.dumps(event) + '\n')

    def queue(self, kind, fields=None, **metadata):
        self.sequence += 1
        name = f'{self.sequence:07d}-{kind}.txt'
        content = {'command': kind, **(fields or {})}
        if any('\n' in str(v) or '\r' in str(v) for v in content.values()):
            raise ValueError('Command fields must be single-line values')
        atomic_text(self.auto / 'commands' / name,
                    ''.join(f'{key}={value}\n' for key, value in content.items()))
        self.pending[name] = {'kind': kind, 'queued': time.monotonic(), **metadata}
        return name

    def artifact(self, value, extension):
        if value:
            if Path(value).name != value or any(c in value for c in ':\\/'):
                raise ValueError('Artifact name must be a filename')
            name = value
        else:
            name = dt.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + extension
        return self.auto / 'artifacts' / name

    def action(self, request):
        with self.lock:
            action = request.get('action')
            if action == 'note':
                self.note = str(request.get('text', ''))[:500]
                self.event('note', self.note)
                return {'ok': True}
            if self.process.poll() is not None:
                raise ValueError('Game has exited')
            return self._dispatch_action(action, request)

    def _dispatch_action(self, action, request):
        if action == 'pad':
            if self.last_status.get('state') != 'running':
                raise ValueError('Wait for a running game before pressing controls')
            if self.lease.deadline is not None or any(p.get('press') for p in self.pending.values()):
                raise ValueError('An input is active; send neutral first or wait for release')
            controls, seconds = validate_pad(request.get('controls', {}), request.get('seconds', .5))
            name = self.queue('pad', {'port': 0, **controls}, press=controls, seconds=seconds)
            self.lease.arm(seconds + 3, time.monotonic())
            self.event('queued', f'{controls} for {seconds:g}s')
        elif action == 'neutral':
            self.lease.deadline = None
            for pending in self.pending.values():
                pending.pop('press', None)
            name = self.queue('pad', {'port': 0}, release=True)
            self.event('queued', 'Release all controls')
        elif action == 'load':
            path = Path(str(request['path'])).resolve()
            if not path.is_relative_to(WORK) or not path.is_file():
                raise ValueError('Load requires an existing checkpoint inside workstation')
            self.action({'action': 'neutral'})
            name = self.queue('load_state', {'path': path})
            self.event('queued', f'Load checkpoint {path.name}')
        elif action in ('save', 'screenshot', 'peek'):
            path = self.artifact(request.get('name'),
                                 {'save': '.sav', 'screenshot': '.png', 'peek': '.bin'}[action])
            fields = {'path': path}
            if action == 'peek':
                address = int(str(request['address']), 0)
                size = int(request.get('size', 4))
                if not 0 < size <= 16777216 or not 0 <= address <= 0x100000000 - size:
                    raise ValueError('Invalid guest memory range')
                fields.update(address=hex(address), size=size)
            name = self.queue({'save': 'save_state', 'screenshot': 'screenshot',
                               'peek': 'read_memory'}[action], fields)
            self.event('queued', f'{action}: {path.name}')
            return {'ok': True, 'command': name, 'path': str(path)}
        elif action == 'write_memory':
            address = int(str(request['address']), 0)
            data_hex = str(request['data']).replace(' ', '')
            if not data_hex or len(data_hex) % 2 != 0:
                raise ValueError('data must be an even-length hex string')
            name = self.queue('write_memory', {'address': hex(address), 'data': data_hex})
            self.event('queued', f'write_memory @ {hex(address)}: {data_hex[:16]}{"…" if len(data_hex) > 16 else ""}')
        elif action in ('pause', 'resume', 'stop'):
            if action in ('pause', 'stop'):
                self.action({'action': 'neutral'})
            name = self.queue(action)
            self.stopping = action == 'stop'
            self.event('queued', action)
        else:
            raise ValueError('Unknown action')
        return {'ok': True, 'command': name}

    def tick(self, now):
        with self.lock:
            path = self.auto / 'status.txt'
            try:
                data = dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)
                if 'frame_count' in data:
                    self.last_status = data
                    try:
                        self.fps_history.append({
                            't': time.time(),
                            'fps': float(data.get('fps', 0)),
                            'vps': float(data.get('vps', 0)),
                        })
                    except (ValueError, TypeError):
                        pass
            except OSError:
                pass
            if self.lease.expire(now):
                for pending in self.pending.values():
                    pending.pop('press', None)
                self.queue('pad', {'port': 0}, release=True)
                self.event('release', 'Timed hold expired; neutral queued')
            for name, pending in list(self.pending.items()):
                failed = (self.auto / 'failed' / name).exists()
                if failed or (self.auto / 'processed' / name).exists():
                    self.pending.pop(name)
                    if failed:
                        self.event('error', f'{name}: {self.last_status.get("last_error", "failed")}')
                    elif 'press' in pending:
                        self.controls = pending['press']
                        self.lease.arm(pending['seconds'], now)
                        self.event('applied', f'Input active: {self.controls}')
                    elif pending.get('release'):
                        self.controls = {}
                        self.event('applied', 'Controls neutral')
                    elif not pending.get('auto'):
                        self.event('applied', name)
                elif now - pending['queued'] > 15 and not pending.get('warned'):
                    pending['warned'] = True
                    self.event('warning', f'Runtime acknowledgement delayed: {name}')
            running = self.last_status.get('state') == 'running' and self.process.poll() is None
            if running and not self.stopping and now >= self.next_capture and not self.pending:
                self.queue('screenshot', {'path': self.auto / 'live.png'}, auto=True)
                self.next_capture = now + 5
            try:
                path = self.auto / 'live.png'
                modified = path.stat().st_mtime
                if modified != self.image_mtime:
                    image = path.read_bytes()
                    if image.startswith(b'\x89PNG\r\n\x1a\n') and image.endswith(b'IEND\xaeB`\x82'):
                        self.image, self.image_mtime = image, modified
                        self.image_time = dt.datetime.fromtimestamp(modified).isoformat(timespec='seconds')
            except OSError:
                pass
            if now >= self.next_sample:
                with (self.session / 'metrics.jsonl').open('a') as file:
                    file.write(json.dumps({'wall_time': time.time(), **self.last_status,
                                           'controls': self.controls}) + '\n')
                self.next_sample = now + 1

    def status(self):
        with self.lock:
            try:
                age = time.time() - (self.auto / 'status.txt').stat().st_mtime
            except OSError:
                age = None
            return {'runtime': self.last_status, 'controls': self.controls,
                    'remaining': max(0, self.lease.deadline - time.monotonic()) if self.lease.deadline else 0,
                    'pending': list(self.pending), 'events': list(self.events), 'note': self.note,
                    'pid': self.process.pid, 'exit_code': self.process.poll(), 'status_age': age,
                    'image_time': self.image_time, 'exclusive': True, 'session': str(self.session),
                    'stdout_path': str(self.session / 'stdout.txt'),
                    'stderr_path': str(self.session / 'stderr.txt')}

    def fps_history_snapshot(self):
        with self.lock:
            return list(self.fps_history)


# ── AttachController (interactive session, no owned process) ─────────────────

class AttachController:
    """Attaches dev console to an already-running interactive session."""

    def __init__(self, auto_dir: Path, furonlog_path: Path):
        self.auto = auto_dir
        self.furonlog_path = furonlog_path
        self.lock = threading.RLock()
        self.events: collections.deque = collections.deque(maxlen=60)
        self.pending: dict = {}
        self.controls: dict = {}
        self.lease = PadLease()
        self.sequence = 0
        self.stopping = False
        self.note = 'Attached to interactive session.'
        self.last_status: dict = {}
        self.fps_history: collections.deque = collections.deque(maxlen=120)
        self.next_capture = 0
        self.next_sample = 0
        self.image = b''
        self.image_time = None
        self.image_mtime = 0
        self.started = time.time()

    def event(self, kind, text):
        event = {'at': dt.datetime.now().isoformat(timespec='seconds'), 'kind': kind, 'text': text}
        self.events.appendleft(event)

    def queue(self, kind, fields=None, **metadata):
        self.sequence += 1
        name = f'{self.sequence:07d}-{kind}.txt'
        content = {'command': kind, **(fields or {})}
        if any('\n' in str(v) or '\r' in str(v) for v in content.values()):
            raise ValueError('Command fields must be single-line values')
        atomic_text(self.auto / 'commands' / name,
                    ''.join(f'{key}={value}\n' for key, value in content.items()))
        self.pending[name] = {'kind': kind, 'queued': time.monotonic(), **metadata}
        return name

    def artifact(self, value, extension):
        if value:
            if Path(value).name != value or any(c in value for c in ':\\/'):
                raise ValueError('Artifact name must be a filename')
            name = value
        else:
            name = dt.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + extension
        artifacts = self.auto / 'artifacts'
        artifacts.mkdir(parents=True, exist_ok=True)
        return artifacts / name

    def action(self, request):
        with self.lock:
            action = request.get('action')
            if action == 'note':
                self.note = str(request.get('text', ''))[:500]
                self.event('note', self.note)
                return {'ok': True}
            return self._dispatch_action(action, request)

    def _dispatch_action(self, action, request):
        # Same dispatch logic as Controller but no process guard
        if action == 'pad':
            if self.last_status.get('state') != 'running':
                raise ValueError('Wait for a running game before pressing controls')
            controls, seconds = validate_pad(request.get('controls', {}), request.get('seconds', .5))
            name = self.queue('pad', {'port': 0, **controls}, press=controls, seconds=seconds)
            self.lease.arm(seconds + 3, time.monotonic())
            self.event('queued', f'{controls} for {seconds:g}s')
        elif action == 'neutral':
            self.lease.deadline = None
            for pending in self.pending.values():
                pending.pop('press', None)
            name = self.queue('pad', {'port': 0}, release=True)
            self.event('queued', 'Release all controls')
        elif action == 'load':
            path = Path(str(request['path'])).resolve()
            if not path.is_relative_to(WORK) or not path.is_file():
                raise ValueError('Load requires an existing checkpoint inside workstation')
            self.queue('pad', {'port': 0}, release=True)
            name = self.queue('load_state', {'path': path})
            self.event('queued', f'Load checkpoint {path.name}')
        elif action in ('save', 'screenshot', 'peek'):
            path = self.artifact(request.get('name'),
                                 {'save': '.sav', 'screenshot': '.png', 'peek': '.bin'}[action])
            fields = {'path': path}
            if action == 'peek':
                address = int(str(request['address']), 0)
                size = int(request.get('size', 4))
                if not 0 < size <= 16777216 or not 0 <= address <= 0x100000000 - size:
                    raise ValueError('Invalid guest memory range')
                fields.update(address=hex(address), size=size)
            name = self.queue({'save': 'save_state', 'screenshot': 'screenshot',
                               'peek': 'read_memory'}[action], fields)
            self.event('queued', f'{action}: {path.name}')
            return {'ok': True, 'command': name, 'path': str(path)}
        elif action == 'write_memory':
            address = int(str(request['address']), 0)
            data_hex = str(request['data']).replace(' ', '')
            if not data_hex or len(data_hex) % 2 != 0:
                raise ValueError('data must be an even-length hex string')
            name = self.queue('write_memory', {'address': hex(address), 'data': data_hex})
            self.event('queued', f'write_memory @ {hex(address)}: {data_hex[:16]}')
        elif action in ('pause', 'resume', 'stop'):
            if action in ('pause', 'stop'):
                self.queue('pad', {'port': 0}, release=True)
            name = self.queue(action)
            self.stopping = action == 'stop'
            self.event('queued', action)
        else:
            raise ValueError('Unknown action')
        return {'ok': True, 'command': name}

    def tick(self, now):
        with self.lock:
            path = self.auto / 'status.txt'
            try:
                data = dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)
                if 'frame_count' in data:
                    self.last_status = data
                    try:
                        self.fps_history.append({
                            't': time.time(),
                            'fps': float(data.get('fps', 0)),
                            'vps': float(data.get('vps', 0)),
                        })
                    except (ValueError, TypeError):
                        pass
            except OSError:
                pass
            if self.lease.expire(now):
                for pending in self.pending.values():
                    pending.pop('press', None)
                self.queue('pad', {'port': 0}, release=True)
                self.event('release', 'Timed hold expired; neutral queued')
            for name, pending in list(self.pending.items()):
                failed = (self.auto / 'failed' / name).exists()
                if failed or (self.auto / 'processed' / name).exists():
                    self.pending.pop(name)
                    if failed:
                        self.event('error', f'{name}: {self.last_status.get("last_error", "failed")}')
                    elif 'press' in pending:
                        self.controls = pending['press']
                        self.lease.arm(pending['seconds'], now)
                        self.event('applied', f'Input active: {self.controls}')
                    elif pending.get('release'):
                        self.controls = {}
                        self.event('applied', 'Controls neutral')
                    elif not pending.get('auto'):
                        self.event('applied', name)
                elif now - pending['queued'] > 15 and not pending.get('warned'):
                    pending['warned'] = True
                    self.event('warning', f'Runtime acknowledgement delayed: {name}')
            # Periodic screenshots in attach mode
            running = self.last_status.get('state') == 'running'
            if running and not self.stopping and now >= self.next_capture and not self.pending:
                self.queue('screenshot', {'path': self.auto / 'artifacts' / 'live.png'}, auto=True)
                self.next_capture = now + 5
            try:
                img_path = self.auto / 'artifacts' / 'live.png'
                modified = img_path.stat().st_mtime
                if modified != self.image_mtime:
                    image = img_path.read_bytes()
                    if image.startswith(b'\x89PNG\r\n\x1a\n') and image.endswith(b'IEND\xaeB`\x82'):
                        self.image, self.image_mtime = image, modified
                        self.image_time = dt.datetime.fromtimestamp(modified).isoformat(timespec='seconds')
            except OSError:
                pass

    def status(self):
        with self.lock:
            try:
                age = time.time() - (self.auto / 'status.txt').stat().st_mtime
            except OSError:
                age = None
            return {'runtime': self.last_status, 'controls': self.controls,
                    'remaining': max(0, self.lease.deadline - time.monotonic()) if self.lease.deadline else 0,
                    'pending': list(self.pending), 'events': list(self.events), 'note': self.note,
                    'pid': None, 'exit_code': None, 'status_age': age,
                    'image_time': self.image_time, 'exclusive': False, 'session': str(self.auto),
                    'stdout_path': None, 'stderr_path': None}

    def fps_history_snapshot(self):
        with self.lock:
            return list(self.fps_history)


# ── HTTP server ───────────────────────────────────────────────────────────────

def build_handler(controller, token, session_dir_for_stdout):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def respond(self, code, body, mime='application/json'):
            if isinstance(body, str):
                body = body.encode('utf-8')
            self.send_response(code)
            self.send_header('Content-Type', mime)
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            raw_path = self.path.split('?', 1)
            path = raw_path[0]
            query = raw_path[1] if len(raw_path) > 1 else ''
            params = {}
            for part in query.split('&'):
                if '=' in part:
                    k, v = part.split('=', 1)
                    params[k] = v

            if path == '/':
                html = Path(__file__).with_name('game_controller.html').read_text(encoding='utf-8')
                html = html.replace('__DEV_TOKEN__', token, 1)
                self.respond(200, html.encode('utf-8'), 'text/html; charset=utf-8')

            elif path == '/api/status':
                self.respond(200, json.dumps(controller.status()).encode())

            elif path == '/live.png':
                with controller.lock:
                    image = controller.image
                self.respond(200 if image else 404, image or b'', 'image/png')

            elif path == '/api/fps_history':
                self.respond(200, json.dumps(controller.fps_history_snapshot()).encode())

            elif path == '/api/stdout':
                n = int(params.get('lines', 300))
                stdout_path = None
                if session_dir_for_stdout and Path(session_dir_for_stdout).is_file():
                    stdout_path = Path(session_dir_for_stdout)
                elif hasattr(controller, 'session'):
                    stdout_path = controller.session / 'stdout.txt'
                lines = read_tail(stdout_path, n) if stdout_path else []
                self.respond(200, json.dumps(lines).encode())

            elif path == '/api/furonlog':
                n = int(params.get('lines', 300))
                self.respond(200, json.dumps(read_tail(FURONLOG, n)).encode())

            elif path == '/api/checkpoints':
                self.respond(200, json.dumps(list_checkpoints()).encode())

            elif path == '/api/artifact':
                name = params.get('name', '')
                if not name or '/' in name or '\\' in name or '..' in name or name.startswith('.'):
                    self.respond(400, b'{"error":"Bad name"}')
                    return
                try:
                    artifacts_dir = (controller.auto / 'artifacts').resolve()
                    artifact_file = (artifacts_dir / name).resolve()
                    if not artifact_file.is_relative_to(artifacts_dir):
                        self.respond(403, b'{"error":"Forbidden"}')
                        return
                    data = artifact_file.read_bytes()
                    mime = 'image/png' if artifact_file.suffix == '.png' else 'application/octet-stream'
                    self.respond(200, data, mime)
                except OSError:
                    self.respond(404, b'{"error":"Not found"}')

            else:
                self.respond(404, b'{}')

        def do_POST(self):
            if self.path != '/api/action' or not secrets.compare_digest(
                    self.headers.get('Authorization', ''), 'Bearer ' + token):
                self.respond(403, b'{"error":"Unauthorized"}')
                return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 16384:
                    raise ValueError('Invalid request length')
                request = json.loads(self.rfile.read(length))
                self.respond(200, json.dumps(controller.action(request)).encode())
            except (ValueError, KeyError, TypeError) as error:
                self.respond(400, json.dumps({'error': str(error)}).encode())

    return Handler


def run_server(controller, token, port, session_dir_for_stdout=None):
    Handler = build_handler(controller, token, session_dir_for_stdout)
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    url = f'http://127.0.0.1:{server.server_port}'
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, url


# ── serve mode ───────────────────────────────────────────────────────────────

def serve(args):
    if ACTIVE.exists():
        try:
            old = json.loads(ACTIVE.read_text())
            with urlopen(old['url'] + '/api/status', timeout=2):
                raise SystemExit('A controller is already serving; send stop before starting another.')
        except OSError:
            pass
    if args.load and (not Path(args.load).is_file() or not Path(args.load).resolve().is_relative_to(WORK)):
        raise SystemExit('Checkpoint must exist inside workstation')
    stamp = dt.datetime.now().strftime('%Y%m%d-%H%M%S')
    session = WORK / 'logs' / ('controller-' + stamp)
    for folder in ('commands', 'artifacts'):
        (session / 'automation' / folder).mkdir(parents=True)
    user = session / 'user'
    exclusive_profile(user)
    pkg = WORK / 'test-agent-controller'
    exe = pkg / 'dahbwu.exe'
    if not exe.is_file():
        raise SystemExit(f'Controller runner must be staged at {exe}')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('BIGWILLY_', 'STATICRECOMP_'))}
    env.update(STATICRECOMP_AUTOMATION_HARD_OVERRIDE='1', STATICRECOMP_AUTOMATION_EXCLUSIVE='1',
               STATICRECOMP_FORCE_NUNCHUK='1')
    command = [str(exe),
               '--game', str(WORK / 'game'),
               '--module', str(WORK / 'build/game-module/gRDHE78_recomp.dll'),
               '--user-dir', str(user),
               '--automation-dir', str(session / 'automation'),
               '--graphics', 'D3D',
               '--background-input', '--mute', '--no-mods',
               '--title', 'Big Willy | Agent control - watch live']
    stdout_file = (session / 'stdout.txt').open('w')
    stderr_file = (session / 'stderr.txt').open('w')
    proc = subprocess.Popen(command, cwd=pkg, env=env, stdout=stdout_file, stderr=stderr_file,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    controller = Controller(session, proc)
    token = secrets.token_urlsafe(32)
    server, url = run_server(controller, token, args.port, str(session / 'stdout.txt'))
    manifest = {'url': url, 'token': token, 'session': str(session), 'game_pid': proc.pid,
                'controller_pid': os.getpid()}
    atomic_text(ACTIVE, json.dumps(manifest, indent=2))
    (session / 'launch.json').write_text(json.dumps(
        {'args': command, 'env': {k: v for k, v in env.items()
                                  if k.startswith(('BIGWILLY_', 'STATICRECOMP_'))},
         'url': url, 'pid': proc.pid}, indent=2))
    print(json.dumps({'url': url, 'session': str(session), 'game_pid': proc.pid}), flush=True)
    loaded = False
    try:
        while proc.poll() is None:
            controller.tick(time.monotonic())
            if not loaded and controller.last_status.get('state') == 'running':
                loaded = True
                controller.action({'action': 'neutral'})
                if args.load:
                    controller.action({'action': 'load', 'path': str(Path(args.load).resolve())})
                controller.action({'action': 'note',
                                   'text': 'Agent controller ready. Hardware input disabled.'})
            time.sleep(.02)
    finally:
        if proc.poll() is None:
            controller.action({'action': 'stop'})
            try:
                proc.wait(15)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(10)
        controller.event('exit', f'Game exit code {proc.returncode}')
        controller.tick(time.monotonic())
        (session / 'final-status.json').write_text(json.dumps(controller.status(), indent=2))
        server.shutdown()
        stdout_file.close()
        stderr_file.close()


# ── attach mode ──────────────────────────────────────────────────────────────

def attach(args):
    auto_dir = Path(args.automation_dir) if args.automation_dir else INTERACTIVE_AUTO
    auto_dir.mkdir(parents=True, exist_ok=True)
    for sub in ('commands', 'processed', 'failed', 'artifacts'):
        (auto_dir / sub).mkdir(parents=True, exist_ok=True)

    furonlog_path = Path(args.furonlog) if getattr(args, 'furonlog', None) else FURONLOG

    controller = AttachController(auto_dir, furonlog_path)
    token = secrets.token_urlsafe(32)
    server, url = run_server(controller, token, args.port)

    manifest = {'url': url, 'token': token, 'session': str(auto_dir),
                'game_pid': None, 'controller_pid': os.getpid(), 'mode': 'attach'}
    atomic_text(ACTIVE, json.dumps(manifest, indent=2))

    print(json.dumps({'url': url, 'session': str(auto_dir), 'mode': 'attach'}), flush=True)
    print(f'Furon Dev Console attached → {url}', flush=True)
    print(f'Automation dir: {auto_dir}', flush=True)
    print(f'Furonlog: {furonlog_path}', flush=True)

    try:
        while True:
            controller.tick(time.monotonic())
            time.sleep(.05)
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='mode', required=True)

    start = sub.add_parser('serve', help='Launch new agent session')
    start.add_argument('--load', help='Checkpoint to load on boot')
    start.add_argument('--port', type=int, default=0)

    att = sub.add_parser('attach', help='Attach dev console to interactive session')
    att.add_argument('--automation-dir', default=None,
                     help=f'Automation dir (default: {INTERACTIVE_AUTO})')
    att.add_argument('--furonlog', default=None,
                     help=f'Furonlog path (default: {FURONLOG})')
    att.add_argument('--port', type=int, default=8080)

    sub.add_parser('status', help='Print current session status')

    send = sub.add_parser('send', help='Send an action to the running session')
    send.add_argument('--action', required=True,
                      choices=['pad', 'neutral', 'load', 'save', 'screenshot', 'peek',
                               'write_memory', 'pause', 'resume', 'stop', 'note'])
    send.add_argument('--seconds', type=float, default=.5)
    send.add_argument('--button', action='append', choices=sorted(BUTTONS), default=[])
    for axis in sorted(AXES):
        send.add_argument('--' + axis.replace('_', '-'), type=float, default=0)
    send.add_argument('--path')
    send.add_argument('--name')
    send.add_argument('--address')
    send.add_argument('--size', type=int, default=4)
    send.add_argument('--data', help='Hex bytes for write_memory')
    send.add_argument('--text')

    args = parser.parse_args()

    if args.mode == 'serve':
        serve(args)
        return
    if args.mode == 'attach':
        attach(args)
        return

    manifest = json.loads(ACTIVE.read_text())

    if args.mode == 'status':
        with urlopen(manifest['url'] + '/api/status', timeout=5) as response:
            print(response.read().decode())
        return

    # send mode
    body = vars(args).copy()
    body['controls'] = {**{axis: getattr(args, axis) for axis in AXES},
                        **{button: 1 for button in args.button}}
    request = Request(manifest['url'] + '/api/action',
                      data=json.dumps(body).encode(),
                      headers={'Authorization': 'Bearer ' + manifest['token'],
                               'Content-Type': 'application/json'})
    with urlopen(request, timeout=5) as response:
        print(response.read().decode())


if __name__ == '__main__':
    main()
