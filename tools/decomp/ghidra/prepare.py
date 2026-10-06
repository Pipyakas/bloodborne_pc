"""Read-only Orbis ELF extraction; private, normalized ELF + seed manifest.

Adds medium-confidence proposed names from research/strings/names.csv
(guessed labels, applied with a guess_ prefix so they stay recognizable
as unverified) and an audit of import-slot coverage.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct


def extract(source, names, link, guessed=None):
    b = source.read_bytes()
    if b[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64')
    phoff = struct.unpack_from('<Q', b, 32)[0]
    entsize, count = struct.unpack_from('<HH', b, 54)
    if entsize != 56:
        raise ValueError('Unexpected program header size')
    ph = [struct.unpack_from('<IIQQQQQQ', b, phoff + i * 56) for i in range(count)]
    eh = next(p for p in ph if p[0] == 0x6474e550)
    if b[eh[2]:eh[2]+4] != bytes([1, 0x1b, 3, 0x3b]):
        raise ValueError('Unsupported EH frame header encoding')
    n = struct.unpack_from('<I', b, eh[2] + 8)[0]
    if 12 + 8 * n > eh[5]:
        raise ValueError('Truncated FDE table')
    starts = [eh[3] + struct.unpack_from('<i', b, eh[2]+12+i*8)[0] for i in range(n)]
    if starts != sorted(set(starts)):
        raise ValueError('FDE starts must be unique and sorted')
    loads = [p for p in ph if p[0] == 1]
    if any(not any(p[1] & 1 and p[3] <= a < p[3]+p[5] for p in loads) for a in starts):
        raise ValueError('FDE start outside executable file-backed memory')
    dyn = next(p for p in ph if p[0] == 2)
    tags = dict(struct.unpack_from('<QQ', b, at) for at in range(dyn[2], dyn[2]+dyn[5], 16))
    blob = next(p for p in ph if p[0] == 0x61000000)[2]
    strings = b[blob+tags[0x61000035]:blob+tags[0x61000035]+tags[0x61000037]]
    symbols = []
    for at in range(blob+tags[0x61000039], blob+tags[0x61000039]+tags[0x6100003f], 24):
        no, info, other, section, value, size = struct.unpack_from('<IBBHQQ', b, at)
        symbols.append((strings[no:strings.index(0, no)].decode('ascii'), section))
    name_map = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', names.read_text()))
    normalized = bytearray(b)
    normalized[7] = 0  # System V OSABI
    struct.pack_into('<H', normalized, 16, 2)  # ET_EXEC: retain image base zero
    # Orbis dynamic tags are not standard ELF relocations. Hide non-load metadata
    # from the generic loader and apply only image-relative pointer relocations.
    for i, p in enumerate(ph):
        if p[0] not in (1, 7, 0x6474e550):
            struct.pack_into('<I', normalized, phoff+i*56, 0)
    slots, relatives = [], 0
    for ot, st in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        for at in range(blob+tags[ot], blob+tags[ot]+tags[st], 24):
            target, info, addend = struct.unpack_from('<QQq', b, at)
            kind, sym = info & 0xffffffff, info >> 32
            if kind == 8:
                p = next(p for p in loads if p[3] <= target and target+8 <= p[3]+p[5])
                struct.pack_into('<Q', normalized, p[2]+target-p[3], addend & ((1<<64)-1))
                relatives += 1
            elif kind in (1, 6, 7) and not symbols[sym][1]:
                nid = symbols[sym][0]
                slots.append(dict(address=hex(target), nid=nid, name=name_map.get(nid, 'nid_'+nid)))
    guessed_map = {}
    if guessed is not None and guessed.exists():
        with guessed.open(encoding='utf-8') as handle:
            for row in csv.DictReader(handle):
                if row['confidence'] != 'medium':
                    continue  # low confidence: not applied
                guessed_map[int(row['function'], 16)] = 'guess_' + re.sub(
                    r'[^A-Za-z0-9_]', '_', row['proposed_name'])
    manifest = dict(sha256=hashlib.sha256(b).hexdigest(), fde_count=n,
                    starts=[hex(a) for a in starts], imports=slots,
                    relative_relocations=relatives, link_summary=json.loads(link.read_text()),
                    import_provenance='ELF Orbis relocations + public src/import_names.inc; link.json summary',
                    guessed_names={hex(a): nm for a, nm in sorted(guessed_map.items())})
    return normalized, manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', type=Path, default=Path(r'C:\code\bloodborne_pc\out\eboot.elf'))
    p.add_argument('--link', type=Path, default=Path(r'C:\code\bloodborne_pc\out\link.json'))
    p.add_argument('--names', type=Path, default=Path(__file__).resolve().parents[3]/'src/import_names.inc')
    p.add_argument('--guessed', type=Path,
                   default=Path(r'C:\code\bbport-decomp\research\strings\names.csv'))
    p.add_argument('--output', type=Path, default=Path(r'C:\code\bbport-decomp\ghidra'))
    a = p.parse_args()
    normalized, manifest = extract(a.elf, a.names, a.link, a.guessed)
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output/'eboot-ghidra.elf').write_bytes(normalized)
    (a.output/'seeds.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps({k:v for k,v in manifest.items()
                      if k not in ('starts','imports','link_summary','guessed_names')}))
    print('Import slots:', len(manifest['imports']),
          'unique NIDs:', len({s['nid'] for s in manifest['imports']}))
    print('Guessed names (medium confidence):', len(manifest['guessed_names']))


if __name__ == '__main__':
    main()
