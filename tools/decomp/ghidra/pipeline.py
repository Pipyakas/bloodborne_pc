"""Serialized, bounded Windows Ghidra runner. No global Java/PATH changes.

Modes:
  analyze          one-time import + seed + auto-analysis (writes the project)
  export ADDR...   export the given hex image-offset entries (resumable)
  export-batch N [FILE]
                   export the next N entries not yet in index.csv, or the
                   entries listed one per line in FILE. --pool game runs
                   game-code starts first (middleware starts last). Large
                   batches pass through a private address file because the
                   Windows command line cannot hold 10k arguments.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(r'C:\code\bbport-decomp\ghidra')
EXPORT_DIR = ROOT/'export'
MIDDLEWARE_CSV = Path(r'C:\code\bbport-decomp\research\middleware\middleware_functions.csv')
SCRIPTS = Path(__file__).resolve().parent


def load_middleware():
    """Image-offset starts attributed to middleware (Havok, FMOD, ...)."""
    import csv
    if not MIDDLEWARE_CSV.exists():
        return set()
    starts = set()
    with MIDDLEWARE_CSV.open(newline='', encoding='utf-8') as handle:
        for row in csv.DictReader(handle):
            starts.add(int(row['address'], 16))
    return starts


def live_pid(pid):
    try:
        subprocess.run(['tasklist', '/fi', 'pid eq %d' % pid, '/nh'],
                       capture_output=True, check=True)
    except subprocess.CalledProcessError:
        return False
    return True


def run(mode, addresses=(), batch=0, batch_file=None, pool='all',
        ghidra=Path(r'C:\code\_tools\ghidra_12.1.4_PUBLIC'),
        java=Path(r'C:\code\_tools\jdk-21.0.12.1+1')):
    ROOT.mkdir(parents=True, exist_ok=True)
    lock = ROOT/'pipeline.lock'
    if lock.exists():
        recorded = lock.read_text().strip()
        if recorded.isdigit() and live_pid(int(recorded)):
            raise SystemExit('Shared project busy: live PID '+recorded)
        print('Removing stale lock (PID %s is not running)' % recorded)
        lock.unlink()
    fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    os.write(fd, str(os.getpid()).encode()); os.close(fd)
    start = time.monotonic()
    stamp = time.strftime('%Y%m%d-%H%M%S')
    log = ROOT/(mode+'-'+stamp+'.log')
    try:
        env = os.environ.copy()
        env.update(JAVA_HOME=str(java), GHIDRA_HEADLESS_MAXMEM='12G',
                   GHIDRA_HEADLESS_JAVA_OPTIONS=(
                       '-XX:ActiveProcessorCount=2 '
                       '-Dapplication.settingsdir=C:/code/bbport-decomp/ghidra/settings '
                       '-Dapplication.cachedir=C:/code/bbport-decomp/ghidra/cache '
                       '-Dapplication.tempdir=C:/code/bbport-decomp/ghidra/temp'))
        command = [str(ghidra/'support/analyzeHeadless.bat'), str(ROOT), 'bbport',
                   '-scriptPath', str(SCRIPTS), '-max-cpu', '2', '-log', str(ROOT/(mode+'-application.log')),
                   '-scriptlog', str(ROOT/(mode+'-scripts.log'))]
        if mode == 'analyze':
            if (ROOT/'bbport.gpr').exists():
                raise RuntimeError('Project exists: refuse overwrite; use a separate private directory for a new import')
            subprocess.run([os.sys.executable, str(SCRIPTS/'prepare.py')], check=True)
            command += ['-import', str(ROOT/'eboot-ghidra.elf'), '-processor', 'x86:LE:64:default',
                        '-cspec', 'gcc', '-preScript', 'SeedFunctions.java', str(ROOT/'seeds.json')]
        elif mode == 'audit':
            count = 0
            if not (ROOT/'bbport.gpr').exists():
                raise RuntimeError('No project: run analyze first')
            command += ['-process', 'eboot-ghidra.elf', '-readOnly', '-noanalysis',
                        '-postScript', 'AuditQuality.java', str(ROOT/'seeds.json'),
                        str(ROOT/'audit.json')]
        else:
            count = len(addresses)
            if batch or batch_file:
                middleware = load_middleware()
                seeds = json.loads((ROOT/'seeds.json').read_text())
                index = EXPORT_DIR/'index.csv'
                done = set()
                if index.exists():
                    for line in index.read_text(encoding='utf-8').splitlines():
                        head = line.split(',', 1)[0]
                        if head and head != 'address':
                            done.add(head.lstrip('0x'))
                if batch_file:
                    pool_lines = [line.strip() for line in batch_file.read_text(encoding='utf-8').splitlines()
                                  if line.strip()]
                else:
                    pool_lines = seeds['starts']
                    if pool == 'game':
                        pool_lines = [a for a in pool_lines
                                      if int(a, 16) not in middleware]
                pending = [a for a in pool_lines if a.lstrip('0x') not in done]
                if batch:
                    addresses = pending[:batch]
                    # Ghidra truncates concatenated script arguments
                    # at 255 characters, so large batches pass through
                    # a fixed short address file ('@' prefix tells the
                    # Java script to read it).
                    if len(addresses) > 400:
                        list_file = ROOT/'list.txt'
                        list_file.write_text('\n'.join(addresses), encoding='utf-8')
                        addresses = ['@' + str(list_file)]
                else:
                    addresses = pending
                if not addresses:
                    print('Nothing pending; export index is complete')
                    return
                count = len(addresses)
                if count and str(addresses[0]).startswith('@'):
                    count = len(list_file.read_text(encoding='utf-8').splitlines())
                print('Export batch: %d of %d pending' % (count, len(pending)))
            command += ['-process', 'eboot-ghidra.elf', '-readOnly', '-noanalysis',
                        '-postScript', 'ExportFunctions.java', str(EXPORT_DIR), *addresses]
        with log.open('w') as output:
            result = subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT)
        text = log.read_text(errors='replace')
        success = result.returncode == 0 and 'REPORT: ' in text and not any(
            s in text for s in ('Error running script', 'SCRIPT ERROR', 'Exception in thread'))
        if mode == 'analyze':
            success = success and 'BBPORT seed total=162959 failed=0' in text \
                and 'Analysis succeeded for file' in text and 'Import succeeded' in text
        elif mode == 'audit':
            success = success and 'BBPORT audit ' in text
        else:
            success = success and 'BBPORT export batch' in text
        status = dict(mode=mode, seconds=time.monotonic()-start, returncode=result.returncode,
                      success=success, functions=count, log=str(log))
        (ROOT/(mode+'-status.json')).write_text(json.dumps(status, indent=2))
        print(json.dumps(status, indent=2))
        if not success:
            raise SystemExit('Ghidra did not complete successfully; inspect '+str(log))
    finally:
        lock.unlink()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['analyze', 'audit', 'export', 'export-batch'])
    p.add_argument('addresses', nargs='*')
    p.add_argument('--batch', type=int, default=0,
                   help='export-batch: how many pending entries to run')
    p.add_argument('--batch-file', type=Path,
                   help='export-batch: entries to run, one hex per line')
    p.add_argument('--pool', choices=['all', 'game'], default='all',
                   help='export-batch pool: all FDE starts, or '
                        'game-code first (excluding middleware starts '
                        'from research/middleware/middleware_functions.csv)')
    p.add_argument('--ghidra', type=Path, default=Path(r'C:\code\_tools\ghidra_12.1.4_PUBLIC'))
    p.add_argument('--java', type=Path, default=Path(r'C:\code\_tools\jdk-21.0.12.1+1'))
    a = p.parse_args()
    run(a.mode, a.addresses, a.batch, a.batch_file, a.pool, a.ghidra, a.java)
