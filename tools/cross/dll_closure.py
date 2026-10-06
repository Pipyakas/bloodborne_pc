"""Copies the DLLs that the given Windows executables import (recursively) from an MSYS2 bin
directory: dll_closure.py <msys bin> <destination> <exe>... System DLLs (not found there) are
left to Windows."""
import pathlib
import re
import shutil
import subprocess
import sys

msys = pathlib.Path(sys.argv[1])
dest = pathlib.Path(sys.argv[2])
dest.mkdir(parents=True, exist_ok=True)
available = {p.name.lower(): p for p in msys.glob('*.dll')}
pending = [pathlib.Path(p) for p in sys.argv[3:]]
seen = set()
while pending:
    image = pending.pop()
    dump = subprocess.run(['llvm-objdump', '-p', str(image)], capture_output=True, text=True, check=True).stdout
    for name in re.findall(r'DLL Name: (\S+)', dump):
        key = name.lower()
        if key in seen or key not in available:
            continue
        seen.add(key)
        shutil.copy2(available[key], dest / available[key].name)
        pending.append(available[key])
print(f'{len(seen)} DLLs -> {dest}')
