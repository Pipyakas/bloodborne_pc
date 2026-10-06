"""Windows headers are case-insensitive; on Linux '#include <Shlobj.h>' misses 'shlobj.h'.
case_includes.py <sysroot include dir> <alias dir> <source dir>...: for every include in the
sources whose exact name is missing from the sysroot but exists in another case, adds a symlink
with the spelling the source uses (the alias dir goes on the compilers' -isystem path)."""
import os
import pathlib
import re
import sys

include = pathlib.Path(sys.argv[1])
alias = pathlib.Path(sys.argv[2])
alias.mkdir(parents=True, exist_ok=True)
lower = {}
for path in include.rglob('*'):
    if path.is_file():
        lower.setdefault(str(path.relative_to(include)).lower(), path)
wanted = set()
pattern = re.compile(rb'#\s*include\s*[<"]([^>"]+)[>"]')
for root in sys.argv[3:]:
    for dirpath, _, files in os.walk(root):
        for name in files:
            if name.endswith(('.c', '.cc', '.cpp', '.h', '.hpp', '.inl')):
                for m in pattern.finditer(pathlib.Path(dirpath, name).read_bytes()):
                    wanted.add(m.group(1).decode(errors='replace'))
made = 0
for name in sorted(wanted):
    if (include / name).exists() or name.lower() not in lower:
        continue
    link = alias / name
    link.parent.mkdir(parents=True, exist_ok=True)
    if not link.exists():
        link.symlink_to(lower[name.lower()])
        made += 1
print(f'{made} case aliases in {alias}')
