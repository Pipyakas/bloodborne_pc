#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Background 60/90/120 FPS movement comparison with PRIVATE save copies.

Uses BB_CAMERA_TRACE; camera displacement is a proxy, not player/physics telemetry.
Requires an unobstructed route and a save that the game's Continue action loads.
No foreground window, audio, online reference-game launch, or changes to source saves.
"""
import argparse
import json
import math
from pathlib import Path
import re
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'mcp'))
import bbport_mcp as mcp

TRACE = re.compile(r'Camera trace: frame=(\d+) ns=(\d+) xyz=([^\n]+)')


def motion_summary(log, start_ns, end_ns):
    rows = []
    for frame, ns, xyz in TRACE.findall(log):
        point = tuple(float(v) for v in xyz.split(','))
        if len(point) == 3 and all(math.isfinite(v) for v in point) and start_ns <= int(ns) <= end_ns:
            rows.append((int(frame), int(ns), point))
    if len(rows) < 2:
        raise ValueError('not enough camera samples in the movement interval')
    elapsed = (rows[-1][1] - rows[0][1]) / 1e9
    if elapsed <= 0:
        raise ValueError('camera samples did not advance')
    distance = math.dist(rows[0][2], rows[-1][2])
    path = sum(math.dist(a[2], b[2]) for a, b in zip(rows, rows[1:]))
    return {'samples': len(rows), 'seconds': elapsed,
            'observed_scene_fps': (rows[-1][0] - rows[0][0]) / elapsed,
            'camera_displacement': distance, 'camera_speed': distance / elapsed,
            'camera_path_speed': path / elapsed}


def run(args):
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for fps in args.fps:
        folder = args.output_dir / str(fps)
        # Do not mix an earlier test's modified save into a fresh comparison.
        folder.mkdir(exist_ok=False)
        shutil.copytree(args.data_dir / 'user', folder / 'user')
        settings = {}
        for line in (args.data_dir / 'bbport.ini').read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep and not key.startswith('#'):
                settings[key.strip()] = value.strip()
        settings.update(upscaler='fsr3', preset='3', frame_gen='off',
                        dynamic_resolution='0', output_res='1920x1080',
                        frame_limit=str(fps), fullscreen='0', maximized='0',
                        launch='continue', live_resolution='1')
        config = folder / 'bbport.ini'
        config.write_text(''.join(f'{k}={v}\n' for k, v in settings.items()))
        mcp.OUT = folder / 'control'
        mcp.LOG = mcp.OUT / 'game.log'
        mcp.SHOTS = mcp.OUT / 'shots'
        mcp.SESSION = mcp.OUT / 'session.json'
        game = mcp.Game()
        try:
            game.launch(fps_limit=fps, game_dir=str(args.game_dir), timeout=120,
                        env={'BB_PROBE': str(args.probe), 'BB_DATA_DIR': str(folder),
                             'BB_CONFIG': str(config), 'BB_USER_DIR': str(folder / 'user'),
                             'BB_FPS': 'uncap', 'BB_VBLANK_HZ': '120', 'BB_LIVE_RES': '1',
                             'BB_CAMERA_TRACE': '4', 'BB_FRAME_GEN': 'off',
                             'BB_UPSCALER': 'fsr3', 'BB_DYNAMIC_RES': '0', 'BB_LAUNCH': 'continue'})
            deadline = time.monotonic() + 90
            while not TRACE.search(mcp.read_log()):
                if not game.running() or time.monotonic() >= deadline:
                    raise RuntimeError('Continue did not reach a scene with camera telemetry')
                time.sleep(0.5)
            game.command(f'wait {int(fps * args.settle_seconds)}', timeout=90)
            game.command('pad ' + args.tokens)
            start = time.monotonic_ns()
            time.sleep(args.seconds)
            end = time.monotonic_ns()
            game.command('pad')
            game.command(f'wait {fps}', timeout=60)
            # Ignore initial acceleration/camera catch-up, not input elapsed time.
            summary = motion_summary(mcp.read_log(), start + 500_000_000, end - 100_000_000)
            summary['requested_fps'] = fps
            summary['tokens'] = args.tokens
            results.append(summary)
            print(json.dumps(summary), flush=True)
        finally:
            if game.running():
                try:
                    game.command('pad')
                finally:
                    game.stop()
    (args.output_dir / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--data-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--probe', type=Path, default=ROOT / 'out' / 'bb-probe')
    parser.add_argument('--fps', type=int, nargs='+', default=[60, 90, 120])
    parser.add_argument('--seconds', type=float, default=2.0)
    parser.add_argument('--settle-seconds', type=float, default=5.0)
    parser.add_argument('--tokens', default='ly=0 circle')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 30 or not 1 <= args.settle_seconds <= 30 or any(f < 30 or f > 120 for f in args.fps):
        parser.error('use 1–30 seconds for movement/settling and 30–120 FPS')
    run(args)
