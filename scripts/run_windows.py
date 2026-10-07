#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows counterpart of run.sh: prepare the game image, compile the patches and start
out/bb-probe.exe. Same environment variables and bbport.ini settings as run.sh.

    run.bat [--game-dir DIR] [bb-probe options...]

The game folder: --game-dir, else BB_GAME_DIR, else the last one used (out/game_dir.txt), else
../CUSA03173 (as run.sh). When none of them holds the game, the first-launch screen
(bb-probe --first-run) asks for the folder or installs the game's .pkg files. Unless BB_PREBUILT=1
the port is (re)built first through MSYS2 (build.sh in the CLANG64 environment)."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
from mods import remove_overlay

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / 'scripts'
PYTHON = sys.executable


def msys_root():
    return Path(os.environ.get('BB_MSYS2', r'C:\msys64'))


def run(arguments, capture=False, check=True, env=None):
    result = subprocess.run([str(a) for a in arguments], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE if capture else None, text=True)
    if check and result.returncode:
        sys.exit(result.returncode)
    return result.stdout.strip() if capture else result.returncode


def build():
    """build.sh in a CLANG64 login shell (its clang, cmake, ninja and pkg-config)."""
    bash = msys_root() / 'usr/bin/bash.exe'
    if not bash.is_file():
        sys.exit(f'MSYS2 not found at {msys_root()} (set BB_MSYS2, or BB_PREBUILT=1 with a built out/)')
    env = dict(os.environ, MSYSTEM='CLANG64', CHERE_INVOKING='1')
    run([bash, '-lc', 'bash build.sh'], env=env)


def with_msys_path(env):
    """MSYS2's DLLs (libc++, SDL3, FFmpeg, ...). System32 is searched before PATH, so the
    Vulkan loader stays the one installed with the GPU driver."""
    return dict(env, PATH=os.pathsep.join([str(msys_root() / 'clang64/bin'), env.get('PATH', '')]))


def first_run(out):
    """No game folder yet: bb-probe's first-launch screen (gpu/shim/bbport_first_run.cpp) asks
    for it, or installs the game's .pkg files; the folder, or None when the user quit. Agent runs
    (hidden or minimized) get no screen unless BB_FIRST_RUN_SCRIPT drives it."""
    background = os.environ.get('BB_HIDDEN') == '1' or os.environ.get('BB_MINIMIZED') == '1'
    if os.environ.get('BB_FIRST_RUN') == '0' or (background and not os.environ.get('BB_FIRST_RUN_SCRIPT')):
        return None
    probe = ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')
    if not probe.is_file():
        return None
    result = out / 'first_run.txt'
    result.unlink(missing_ok=True)
    print('No game folder yet: first-launch screen', flush=True)
    status = subprocess.call([str(probe), '--first-run', str(result)], cwd=ROOT, env=with_msys_path(os.environ))
    if status != 0 or not result.is_file():
        return None
    return Path(result.read_text(encoding='utf-8').strip())


def settings_value(config, key):
    if not config.is_file():
        return None
    for line in config.read_text(errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return None


def main():
    arguments = sys.argv[1:]
    game = os.environ.get('BB_GAME_DIR')
    if arguments[:1] == ['--game-dir'] and len(arguments) > 1:
        game, arguments = arguments[1], arguments[2:]
    data = Path(os.environ.get('BB_DATA_DIR', ROOT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = Path(os.environ['BB_CONFIG'])
    if 'BB_FSR4_DIR' not in os.environ and not (ROOT / 'fsr4_shaders').is_dir() and (data / 'fsr4_shaders').is_dir():
        os.environ['BB_FSR4_DIR'] = str(data / 'fsr4_shaders')
    if 'BB_FSR411_DIR' not in os.environ and not (ROOT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        os.environ['BB_FSR411_DIR'] = str(data / 'fsr4_411')
    # The last folder that worked is remembered, so run.bat alone starts the game afterwards.
    remembered = out / 'game_dir.txt'
    chosen = bool(game)  # given explicitly: an error rather than the first-launch screen
    if not game and remembered.is_file():
        game = remembered.read_text(encoding='utf-8').strip()
    game = Path(game) if game else ROOT.parent / 'CUSA03173'
    prebuilt = os.environ.get('BB_PREBUILT') == '1'
    if not (game / 'eboot.bin').is_file() and not chosen:
        if not prebuilt:
            build()  # the screen is part of bb-probe
            prebuilt = True
        game = first_run(out) or game
    if not (game / 'eboot.bin').is_file():
        sys.exit(f'No eboot.bin in {game} (pass --game-dir or set BB_GAME_DIR).')
    original = game.resolve()
    remembered.write_text(str(original), encoding='utf-8')
    os.environ['BB_GAME_DIR'] = str(original)
    # The in-game menu's "Apply and restart" runs this launcher again (probe.c runtime_restart).
    os.environ['BB_RESTART_COMMAND'] = subprocess.list2cmdline([PYTHON, str(Path(__file__).resolve()), *sys.argv[1:]])

    merged = Path(run([PYTHON, SCRIPTS / 'mods.py', original, '--out', out,
                       '--mods-dir', os.environ.get('BB_MODS_DIR', data / 'mods'),
                       '--config', os.environ.get('BB_MODS_CONFIG', data / 'mods.json'),
                       '--enabled', os.environ.get('BB_MODS_ENABLED', '1')], capture=True))
    overlay = merged if merged.resolve() != original else None
    try:
        for script, extra in (('prepare.py', []), ('link_libc.py', []), ('link_modules.py', []),
                              ('content_profile.py', ['--sku', os.environ.get('BB_CONTENT_SKU', 'full')])):
            run([PYTHON, SCRIPTS / script, merged, '--out', out, *extra])
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if os.environ.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                os.environ.pop(key, None)
        fps = os.environ.get('BB_FPS', 'uncap')
        scaled_render = scaled_output = None
        if not os.environ.get('BB_RENDER_RES'):
            sizes = run([PYTHON, SCRIPTS / 'patches.py', '--print-scaled', '--settings', config], capture=True, check=False)
            if sizes and len(sizes.split()) == 2:
                scaled_render, scaled_output = sizes.split()
        if not prebuilt:
            build()
        live = '0'
        if scaled_output:
            live = os.environ.get('BB_LIVE_RES') or settings_value(config, 'live_resolution') or 'auto'
            if live == 'auto':
                # Built next to bb-probe.exe (the data directory's out\ need not hold executables).
                caps = (ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')).with_name('bb-gpu-capabilities.exe')
                live = run([caps, '--live-resolution'], capture=True, check=False) or '0'
            live = '1' if live == '1' else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            os.environ.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            os.environ.setdefault('BB_DMEM_MB', '9152')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {os.environ["BB_DMEM_MB"]} MiB '
                  '(live_resolution=1: live changes)')
        run([PYTHON, SCRIPTS / 'patches.py', '--out', out, '--fps', fps, '--extra', os.environ.get('BB_PATCHES', ''),
             '--settings', config, '--game-dir', merged, '--render-res', os.environ.get('BB_RENDER_RES', ''),
             '--output-res', os.environ.get('BB_OUTPUT_RES', ''),
             '--patches-dir', os.environ.get('BB_PATCHES_DIR', data / 'patches'),
             '--patches-config', os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json')])
        os.environ.setdefault('BB_VBLANK_HZ', {'uncap': '0', '90': '90'}.get(fps, '60'))
        probe = ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')
        command = [probe, out / 'boot-linked.bin', '--content-profile', out / 'content.bin',
                   '--patches', out / 'patches.bin', '--app0', merged,
                   '--user', os.environ.get('BB_USER_DIR', data / 'user'),
                   '--timeout', os.environ.get('BB_TIMEOUT', '0'), *arguments]
        os.environ['PATH'] = with_msys_path(os.environ)['PATH']
        print('Starting:', ' '.join(shlex.quote(str(c)) for c in command), flush=True)
        try:
            status = subprocess.call([str(c) for c in command], cwd=ROOT)
        except KeyboardInterrupt:
            status = 130
        return status
    finally:
        if overlay:
            remove_overlay(overlay)


if __name__ == '__main__':
    sys.exit(main())
