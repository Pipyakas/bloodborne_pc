#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""MCP server (stdio, standard library only) that lets agents start and play the port without
touching the desktop: the game runs with BB_HIDDEN=1 (no window, the host keyboard and gamepads
ignored), BB_AUDIO=none and no console, and is driven through its control channel (BB_CONTROL,
src/runtime_control.c). Screenshots come from the presenter, not from the screen.

Registered in the repository's .mcp.json; by hand: python tools/mcp/bbport_mcp.py
The game's output goes to out/mcp/game.log, screenshots to out/mcp/shots/.

Command line (sessions without the MCP server, scripts): one tool per call, the game keeps
running in between (out/mcp/session.json), arguments as key=value (values in JSON when they
parse) or one JSON object:
    python tools/mcp/bbport_mcp.py launch
    python tools/mcp/bbport_mcp.py press tokens=cross repeat=2 screenshot=true
    python tools/mcp/bbport_mcp.py screenshot          (prints the PNG's path)
    python tools/mcp/bbport_mcp.py stop
    python tools/mcp/bbport_mcp.py help"""
import base64
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'out' / 'mcp'
LOG = OUT / 'game.log'
SHOTS = OUT / 'shots'
SESSION = OUT / 'session.json'
WINDOWS = os.name == 'nt'
PROTOCOL = '2025-06-18'


class Failure(Exception):
    pass


class Game:
    """One game process and its control connection."""

    def __init__(self):
        self.process = None
        self.pid = None  # a game started by an earlier command-line call (SESSION)
        self.detached = False  # command line: the game outlives this process
        self.job = None
        self.connection = None
        self.reader = None
        self.port = None
        self.started = 0.0

    # ---- process ----
    def running(self):
        if self.process:
            return self.process.poll() is None
        return bool(self.pid) and pid_alive(self.pid)

    def attach(self):
        """The game an earlier command-line call started, if it still runs."""
        try:
            session = json.loads(SESSION.read_text())
        except (OSError, ValueError):
            return
        self.pid, self.port, self.started = session['pid'], session['port'], session['started']
        if not self.running():
            self.pid = None
            SESSION.unlink(missing_ok=True)

    def launch(self, hidden=True, audio=False, build=False, fps_limit=60, game_dir=None, env=None, timeout=300):
        if self.running():
            raise Failure('the game is already running (game_stop first)')
        OUT.mkdir(parents=True, exist_ok=True)
        environment = dict(os.environ)
        environment.update(BB_CONTROL='0', BB_FRAME_STATS='1')
        if hidden:
            environment['BB_HIDDEN'] = '1'
        if not audio:
            environment['BB_AUDIO'] = 'none'
        if not build:
            environment['BB_PREBUILT'] = '1'
        if fps_limit:
            environment['BB_FPS_LIMIT'] = str(fps_limit)
        if game_dir:
            environment['BB_GAME_DIR'] = game_dir
        environment.update({k: str(v) for k, v in (env or {}).items()})
        if WINDOWS:
            msys = Path(environment.get('BB_MSYS2', r'C:\msys64'))
            command = [str(msys / 'clang64/bin/python.exe'), str(ROOT / 'scripts/run_windows.py')]
        else:
            command = ['bash', str(ROOT / 'run.sh')]
        log = open(LOG, 'wb')
        options = dict(cwd=ROOT, env=environment, stdin=subprocess.DEVNULL, stdout=log,
                       stderr=subprocess.STDOUT)
        if WINDOWS:
            options['creationflags'] = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            options['start_new_session'] = True
        try:
            # Detached: out of the calling tool's job, which may end with the call.
            breakaway = subprocess.CREATE_BREAKAWAY_FROM_JOB if WINDOWS and self.detached else 0
            self.process = subprocess.Popen(command, **dict(options, creationflags=options.get('creationflags', 0) | breakaway))
        except OSError:
            self.process = subprocess.Popen(command, **options)
        log.close()
        self.job = kill_on_close_job(self.process) if WINDOWS and not self.detached else None
        self.started = time.time()
        self.port = None
        deadline = time.monotonic() + timeout
        pattern = re.compile(r'Runtime: control on 127\.0\.0\.1:(\d+)')
        while time.monotonic() < deadline:
            match = pattern.search(read_log())
            if match:
                self.port = int(match.group(1))
                break
            if not self.running():
                raise Failure(f'the game exited ({self.process.returncode}) before its control '
                              f'channel opened:\n{tail(40)}')
            time.sleep(0.25)
        else:
            raise Failure(f'no control channel after {timeout} s:\n{tail(40)}')
        if self.detached:
            SESSION.write_text(json.dumps({'pid': self.process.pid, 'port': self.port, 'started': self.started}))
        self.connect()

    def stop(self):
        if self.connection:
            try:
                self.command('quit', timeout=3)
            except (Failure, OSError):
                pass
            self.disconnect()
        if self.process:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            if WINDOWS and self.job:
                import ctypes
                ctypes.windll.kernel32.TerminateJobObject(self.job, 1)
                ctypes.windll.kernel32.CloseHandle(self.job)
                self.job = None
            elif self.process.poll() is None:
                kill_tree(self.process.pid)
            self.process.wait(timeout=10)
        elif self.pid:
            deadline = time.monotonic() + 5
            while pid_alive(self.pid) and time.monotonic() < deadline:
                time.sleep(0.2)
            if pid_alive(self.pid):
                kill_tree(self.pid)
        code = self.process.returncode if self.process else None
        self.process = self.pid = None
        SESSION.unlink(missing_ok=True)
        return code

    # ---- control channel ----
    def connect(self):
        self.connection = socket.create_connection(('127.0.0.1', self.port), timeout=10)
        self.reader = self.connection.makefile('r', encoding='utf-8', newline='\n')

    def disconnect(self):
        for closable in (self.reader, self.connection):
            try:
                closable and closable.close()
            except OSError:
                pass
        self.connection = self.reader = None

    def command(self, line, timeout=30):
        if not self.connection:
            if not self.running():
                raise Failure('the game is not running (game_launch first)')
            self.connect()
        self.connection.settimeout(timeout)
        try:
            self.connection.sendall((line + '\n').encode())
            reply = self.reader.readline().strip()
        except (OSError, socket.timeout) as error:
            self.disconnect()
            raise Failure(f'control channel: {error}' + ('' if self.running() else f'\nthe game exited:\n{tail(40)}'))
        if not reply:
            self.disconnect()
            raise Failure('control channel closed' + ('' if self.running() else f'; the game exited:\n{tail(40)}'))
        if reply.startswith('error'):
            raise Failure(reply)
        return reply

    def status(self):
        fields = dict(item.split('=', 1) for item in self.command('status').split()[1:])
        return {k: int(v) for k, v in fields.items()}


def kill_on_close_job(process):
    """A job object that ends the launcher and the game with this server (or game_stop)."""
    import ctypes
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                    ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                    ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD),
                    ('SchedulingClass', wintypes.DWORD)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [('BasicLimitInformation', BasicLimits), ('IoInfo', ctypes.c_uint64 * 6),
                    ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                    ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]

    kernel32 = ctypes.windll.kernel32
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    job = kernel32.CreateJobObjectW(None, None)
    limits = ExtendedLimits()
    limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    kernel32.SetInformationJobObject(wintypes.HANDLE(job), 9, ctypes.byref(limits), ctypes.sizeof(limits))
    kernel32.AssignProcessToJobObject(wintypes.HANDLE(job), wintypes.HANDLE(int(process._handle)))
    return job


def pid_alive(pid):
    if not WINDOWS:
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    import ctypes
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    alive = kernel32.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value == 259  # STILL_ACTIVE
    kernel32.CloseHandle(handle)
    return bool(alive)


def kill_tree(pid):
    if WINDOWS:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(pid)], capture_output=True)
    else:
        try:
            os.killpg(pid, 9)
        except OSError:
            pass


def log_size():
    try:
        return LOG.stat().st_size
    except OSError:
        return 0


def read_log(start=0):
    """The log from byte `start` on (it grows by a few lines per second while the game runs)."""
    try:
        with open(LOG, 'rb') as log:
            log.seek(max(0, start))
            return log.read().decode('utf-8', errors='replace')
    except OSError:
        return ''


def tail(lines):
    return '\n'.join(read_log(log_size() - 256 * lines).splitlines()[-lines:])


GAME = Game()

# ---- tools ----
TOKENS = ('Pad tokens, space separated: cross circle square triangle l1 r1 l2 r2 l3 r3 options '
          'touchpad touchpad_left touchpad_right up down left right (d-pad), and sticks '
          'lx= ly= rx= ry= (0..255, 128 = centre; ly=0 is up).')
TOOLS = {}


def tool(name, description, properties=None, required=()):
    def register(function):
        TOOLS[name] = dict(function=function, schema={
            'name': name, 'description': description,
            'inputSchema': {'type': 'object', 'properties': properties or {}, 'required': list(required)}})
        return function
    return register


def content(value):
    return [{'type': 'text', 'text': value}]


def capture(max_width):
    SHOTS.mkdir(parents=True, exist_ok=True)
    taken = [int(p.stem[5:]) for p in SHOTS.glob('shot_*.png') if p.stem[5:].isdigit()]
    path = SHOTS / f'shot_{max(taken, default=0) + 1:04d}.png'
    reply = GAME.command(f'screenshot {int(max_width)} {path}', timeout=15)
    _, width, height = reply.split()
    data = base64.b64encode(path.read_bytes()).decode()
    return [{'type': 'image', 'data': data, 'mimeType': 'image/png'},
            {'type': 'text', 'text': f'{width}x{height}, saved to {path}'}]


@tool('game_launch', 'Start the game in the background (hidden window, no audio, host input '
      'ignored) and wait until it reads the pad, i.e. the title screen is loading. Takes 20-90 s; '
      'with build=true the port is rebuilt first (minutes, added to the timeout). Returns the end of the log.',
      {'hidden': {'type': 'boolean', 'default': True, 'description': 'false shows the window (and takes focus)'},
       'audio': {'type': 'boolean', 'default': False},
       'build': {'type': 'boolean', 'default': False, 'description': 'rebuild through build.sh first'},
       'fps_limit': {'type': 'integer', 'default': 60, 'description': '0 = uncapped (loads the GPU)'},
       'game_dir': {'type': 'string', 'description': 'folder with eboot.bin; default: BB_GAME_DIR, '
                    'else the last one used (out/game_dir.txt)'},
       'env': {'type': 'object', 'additionalProperties': {'type': 'string'},
               'description': 'extra environment, e.g. {"BB_UPSCALER": "taa", "BB_PAD_REPLAY": "route.txt"}'},
       'timeout_s': {'type': 'number', 'default': 300}})
def game_launch(hidden=True, audio=False, build=False, fps_limit=60, game_dir=None, env=None, timeout_s=300):
    timeout_s = float(timeout_s) + (1800 if build else 0)  # the build's link-time optimization
    deadline = time.monotonic() + timeout_s
    try:
        GAME.launch(hidden, audio, build, fps_limit, game_dir, env, timeout_s)
    except Failure:
        if GAME.process:
            GAME.stop()
        raise
    while time.monotonic() < deadline:
        status = GAME.status()
        if status['pad_open']:
            return content(f'Running, control port {GAME.port}, {status}.\n{tail(15)}')
        time.sleep(0.5)
    return content(f'Running, but the pad was not opened yet.\n{tail(30)}')


@tool('game_stop', 'End the game (quit through the control channel, then terminate).')
def game_stop():
    if not GAME.process and not GAME.pid:
        return content('not running')
    code = GAME.stop()
    return content('stopped' + (f' (exit code {code})' if code is not None else ''))


@tool('game_status', 'Whether the game runs, its pad/frame counters, the latest frame statistics '
      'and whether a text entry (name dialog) waits for game_text.')
def game_status():
    if not GAME.running():
        code = GAME.process.returncode if GAME.process else None
        return content(f'not running (exit code {code})\n{tail(20)}')
    stats = [l for l in read_log(log_size() - 65536).splitlines() if l.startswith('Frame stats')]
    return content(f'running {time.time() - GAME.started:.0f} s, {GAME.status()}\n'
                f'{stats[-1] if stats else "no frame statistics yet"}')


@tool('game_screenshot', 'The latest presented frame (the game picture with its HUD and menus) as '
      'a PNG, read back from the GPU: works while the window is hidden.',
      {'max_width': {'type': 'integer', 'default': 1024, 'description': '0 = full size'}})
def game_screenshot(max_width=1024):
    return capture(max_width)


@tool('game_press', 'Press and release: holds the tokens for `frames` game frames (pad reads), '
      'then releases, `repeat` times with `gap_frames` between. Optionally returns a screenshot '
      'taken `settle_frames` after the last release. ' + TOKENS,
      {'tokens': {'type': 'string', 'description': 'e.g. "cross", "l1 r1", "lx=0"'},
       'frames': {'type': 'integer', 'default': 4}, 'repeat': {'type': 'integer', 'default': 1},
       'gap_frames': {'type': 'integer', 'default': 8},
       'screenshot': {'type': 'boolean', 'default': False},
       'settle_frames': {'type': 'integer', 'default': 30},
       'max_width': {'type': 'integer', 'default': 1024}}, ['tokens'])
def game_press(tokens, frames=4, repeat=1, gap_frames=8, screenshot=False, settle_frames=30,
               max_width=1024):
    for i in range(max(1, repeat)):
        if i:
            GAME.command(f'wait {gap_frames}', timeout=gap_frames * 0.2 + 30)
        GAME.command(f'press {max(1, frames)} {tokens}', timeout=frames * 0.2 + 30)
    result = content(f'pressed {tokens} x{max(1, repeat)}')
    if screenshot:
        GAME.command(f'wait {settle_frames}', timeout=settle_frames * 0.2 + 30)
        result += capture(max_width)
    return result


@tool('game_hold', 'Hold tokens until game_release or the next game_hold/game_press (e.g. walk '
      'forward with "ly=0", run with "ly=0 circle"). ' + TOKENS,
      {'tokens': {'type': 'string'}}, ['tokens'])
def game_hold(tokens):
    GAME.command(f'pad {tokens}')
    return content(f'holding {tokens}')


@tool('game_release', 'Release every held button and centre the sticks.')
def game_release():
    GAME.command('pad')
    return content('released')


@tool('game_wait', 'Wait for frames to be presented, for seconds, or until a regular expression '
      'appears in the log (new lines only, unless from_start). Returns the matching line.',
      {'frames': {'type': 'integer'}, 'seconds': {'type': 'number'},
       'log_pattern': {'type': 'string'}, 'from_start': {'type': 'boolean', 'default': False},
       'timeout_s': {'type': 'number', 'default': 120}})
def game_wait(frames=None, seconds=None, log_pattern=None, from_start=False, timeout_s=120):
    if frames:
        return content(GAME.command(f'wait {int(frames)}', timeout=frames * 0.2 + 30))
    if log_pattern:
        pattern = re.compile(log_pattern)
        offset = 0 if from_start else log_size()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            # Whole lines only: a line still being written is matched once it is complete.
            with open(LOG, 'rb') as log:
                log.seek(offset)
                chunk = log.read()
            complete = chunk[:chunk.rfind(b'\n') + 1]
            offset += len(complete)
            for line in complete.decode('utf-8', errors='replace').splitlines():
                if pattern.search(line):
                    return content(line)
            if not GAME.running():
                raise Failure(f'the game exited:\n{tail(30)}')
            time.sleep(0.25)
        raise Failure(f'{log_pattern!r} did not appear within {timeout_s} s')
    time.sleep(min(float(seconds or 1), 600))
    return content('waited')


@tool('game_log', 'The end of the game log (out/mcp/game.log), or only lines matching a regular '
      'expression.', {'lines': {'type': 'integer', 'default': 60}, 'pattern': {'type': 'string'}})
def game_log(lines=60, pattern=None):
    log = read_log().splitlines()
    if pattern:
        expression = re.compile(pattern)
        log = [l for l in log if expression.search(l)]
    return content('\n'.join(log[-lines:]) or '(empty)')


@tool('game_text', 'Confirm the open text entry (e.g. the character name dialog) with this text.',
      {'text': {'type': 'string'}}, ['text'])
def game_text(text):
    GAME.command('text ' + text)
    return content('submitted')


@tool('game_commands', 'Run raw control-channel commands in order (fewer round trips for '
      'scripted routes): "press <frames> <tokens>", "pad [tokens]", "wait <frames>", '
      '"sleep <seconds>", "status". Stops at the first error.',
      {'commands': {'type': 'array', 'items': {'type': 'string'}}}, ['commands'])
def game_commands(commands):
    replies = []
    for line in commands:
        if line.startswith('sleep'):
            time.sleep(min(float(line.split()[1]), 600))
            replies.append(f'{line}: ok')
            continue
        if line.split()[:1] in (['screenshot'], ['quit']):
            raise Failure('use game_screenshot / game_stop')
        try:
            replies.append(f'{line}: {GAME.command(line, timeout=120)}')
        except Failure as error:
            replies.append(f'{line}: {error}')
            break
    return content('\n'.join(replies))


# ---- MCP over stdio (JSON-RPC 2.0, one message per line) ----
def handle(message):
    method, params = message.get('method'), message.get('params') or {}
    if method == 'initialize':
        return {'protocolVersion': params.get('protocolVersion', PROTOCOL),
                'capabilities': {'tools': {}},
                'serverInfo': {'name': 'bbport', 'version': '1.0'},
                'instructions': 'Drives the Bloodborne port in the background: game_launch, then '
                                'game_screenshot / game_press / game_hold / game_wait, game_stop when done.'}
    if method == 'ping':
        return {}
    if method == 'tools/list':
        return {'tools': [t['schema'] for t in TOOLS.values()]}
    if method == 'tools/call':
        entry = TOOLS.get(params.get('name'))
        if not entry:
            raise KeyError(params.get('name'))
        arguments = dict(params.get('arguments') or {})
        try:
            return {'content': entry['function'](**arguments)}
        except Failure as error:
            return {'content': content(str(error)), 'isError': True}
        except Exception as error:  # report, keep serving
            return {'content': content(f'{type(error).__name__}: {error}'), 'isError': True}
    raise KeyError(method)


def cli(arguments):
    name = arguments[0] if arguments[0].startswith('game_') else 'game_' + arguments[0]
    if name not in TOOLS:
        for entry in TOOLS.values():
            schema = entry['schema']
            options = ' '.join(f'{k}=' for k in schema['inputSchema']['properties'])
            print(f"{schema['name'][5:]} {options}\n    {schema['description']}\n")
        return 0 if name == 'game_help' else 2
    if arguments[1:2] and arguments[1].startswith('{'):
        options = json.loads(arguments[1])
    else:
        options = {}
        for item in arguments[1:]:
            key, _, value = item.partition('=')
            try:
                options[key] = json.loads(value)
            except ValueError:
                options[key] = value
    GAME.detached = True
    GAME.attach()
    try:
        result = TOOLS[name]['function'](**options)
    except Failure as error:
        print(error)
        return 1
    finally:
        GAME.disconnect()
    print('\n'.join(c['text'] for c in result if c['type'] == 'text'))
    return 0


def main():
    if len(sys.argv) > 1:
        sys.exit(cli(sys.argv[1:]))
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    try:
        for raw in stdin:
            if not raw.strip():
                continue
            message = json.loads(raw)
            if 'id' not in message:
                continue  # notifications
            try:
                response = {'jsonrpc': '2.0', 'id': message['id'], 'result': handle(message)}
            except KeyError as error:
                response = {'jsonrpc': '2.0', 'id': message['id'],
                            'error': {'code': -32601, 'message': f'unknown: {error}'}}
            stdout.write(json.dumps(response).encode() + b'\n')
            stdout.flush()
    finally:
        if GAME.process:
            GAME.stop()


if __name__ == '__main__':
    main()
