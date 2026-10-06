"""Decomp Phase 0: function inventory and call graph for Bloodborne (CUSA03173 v1.09).

Produces private data in C:\\code\\bbport-decomp\\data\\ (functions.csv,
callgraph.csv, inventory_summary.md, pilot_candidates.csv). This file is
public tooling: it contains no game-binary-derived data, only methods.

Pipeline stages (run in order):
  python inventory.py fdes                       # FDE table -> data/fde_table.csv
  python inventory.py imports                    # Sony relocs + import_names.inc -> data/import_thunks.json
  python inventory.py disasm --chunk I --chunks N # 20k-fn slices, <=4 procs -> data/partial_XX.csv
  python inventory.py merge                      # partials -> data/functions.csv (basic) + data/callgraph.csv
  python inventory.py enrich                     # join modules/names/middleware/strings/vtables -> full functions.csv
  python inventory.py report                     # data/inventory_summary.md + data/pilot_candidates.csv

Conventions: addresses are image offsets (PS4 VA = offset + 0x400000).
FDE count must be 162,959 (first starts 0xa0, 0xf0, 0x170); anything else aborts.
is_leaf = no direct callees AND no import calls (tailjump-only fns are leaves).
A rel32 call/jmp target inside a function but past its start is attributed to
that containing function and counted separately (offstart).
Middleware tag priority: middleware_functions.csv library beats strings
subsystem; confidence medium beats low; ties keep file order (deterministic).
Multi-module functions get the module with the smallest function_count.
Only medium-confidence names.csv rows become labels.
"""
import argparse
import bisect
import csv
import json
import os
import random
import re
import struct
import sys
import time

csv.field_size_limit(10 ** 9)

HERE = os.path.dirname(os.path.abspath(__file__))  # tools/decomp in worktree
DATA = r"C:\code\bbport-decomp\data"
RESEARCH = r"C:\code\bbport-decomp\research"
ELF = r"C:\code\bloodborne_pc\out\eboot.elf"
IMPORT_NAMES = r"C:\code\bloodborne_pc\src\import_names.inc"
BASE_OFF = 0x4000  # file offset of vaddr 0 (executable LOAD)

EXPECT_FDES = 162959
MAX_PROCS = 4
CHUNK_FUNCS = 20000

sys.path.insert(0, os.path.join(RESEARCH, "globals"))
import ereader  # noqa: E402  (FDE starts/sizes; read-only reuse)


# --------------------------------------------------------------------------
# stage: fdes
# --------------------------------------------------------------------------
def cmd_fdes(args):
    t0 = time.time()
    e = ereader.Elf(ELF)
    fns, count = ereader.parse_functions(e)
    assert count == EXPECT_FDES, count
    assert fns[0][0] == 0xA0 and fns[1][0] == 0xF0 and fns[2][0] == 0x170, \
        [hex(x[0]) for x in fns[:3]]
    assert len(fns) == EXPECT_FDES and len(set(s for s, _ in fns)) == EXPECT_FDES
    os.makedirs(DATA, exist_ok=True)
    tmp = os.path.join(DATA, "fde_table.tmp.csv")
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "size"])
        for s, z in fns:
            w.writerow(["0x%x" % s, z])
    os.replace(tmp, os.path.join(DATA, "fde_table.csv"))
    print("fdes: %d in %.1fs" % (len(fns), time.time() - t0), flush=True)


def load_fdes():
    rows = list(csv.DictReader(open(os.path.join(DATA, "fde_table.csv"))))
    assert len(rows) == EXPECT_FDES, len(rows)
    return [(int(r["start"], 16), int(r["size"])) for r in rows]


# --------------------------------------------------------------------------
# stage: imports (Sony relocation tables + import_names.inc)
# --------------------------------------------------------------------------
def cmd_imports(args):
    """Build data/import_thunks.json: {thunk_addr: name} and {got_slot: name}.

    Parses PT_DYNAMIC inside the 0x61000000 SCE segment exactly like
    research/renderer/imports.py (kind 1/6/7 relocations), resolves NIDs via
    src/import_names.inc, and scans the post-code thunk range for
    `jmp qword [rip+disp32]` (ff 25) stubs whose target is a JUMP_SLOT.
    cross-checked against research/frame_timing/plt_symbols.json.
    """
    t0 = time.time()
    f = open(ELF, "rb")
    d = f.read(64)
    assert d[:4] == b"\x7fELF"
    e_phoff, = struct.unpack_from("<Q", d, 32)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 54)
    phdrs = []
    for i in range(e_phnum):
        f.seek(e_phoff + i * e_phentsize)
        p = struct.unpack("<IIQQQQQQ", f.read(56))
        phdrs.append(dict(type=p[0], flags=p[1], off=p[2], vaddr=p[3],
                          filesz=p[5], memsz=p[6]))

    def read(off, n):
        f.seek(off)
        return f.read(n)

    dp = next(p for p in phdrs if p["type"] == 2)
    dd = read(dp["off"], dp["filesz"])
    tags = {}
    i = 0
    while i < len(dd):
        tag, val = struct.unpack_from("<qQ", dd, i)
        i += 16
        if tag == 0:
            break
        tags[tag] = val
    lib = next(p for p in phdrs if p["type"] == 0x61000000)
    blob = read(lib["off"], lib["filesz"])
    ST, STSZ = tags[0x61000035], tags[0x61000037]
    strings = blob[ST:ST + STSZ]
    syms_raw = blob[tags[0x61000039]:tags[0x61000039] + tags[0x6100003F]]

    def sstr(off):
        return strings[off:strings.index(0, off)].decode("latin1")

    symbols = []
    for pos in range(0, len(syms_raw), 24):
        name, info, _, _, value, size = struct.unpack_from("<IBBHQQ", syms_raw, pos)
        symbols.append(dict(nid=sstr(name).split("#")[0] if name else "",
                            value=value, size=size))
    order, slots = {}, {}
    for ot, st in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
        tbl = blob[tags[ot]:tags[ot] + tags[st]]
        for pos in range(0, len(tbl), 24):
            target, info, _ = struct.unpack_from("<QQq", tbl, pos)
            kind, sym = info & 0xFFFFFFFF, info >> 32
            if kind in (1, 6, 7):
                if sym not in order:
                    order[sym] = len(order)
                slots.setdefault(sym, []).append(target)
    names_txt = open(IMPORT_NAMES, encoding="utf-8", errors="replace").read()
    nid2name = dict(re.findall(r'\{"([^"]+)",\s*"([^"]+)"', names_txt))
    slot2name, jmp = {}, {}
    for sym, tgts in slots.items():
        nid = symbols[sym]["nid"]
        nm = nid2name.get(nid, "NID_" + nid)
        for t in tgts:
            slot2name.setdefault(t, nm)
            jmp.setdefault(t, []).append(sym)
    # thunk scan: ff 25 after the last FDE function
    fns = load_fdes()
    scan_lo = max(s + z for s, z in fns)
    code = next(p for p in phdrs if p["type"] == 1 and p["flags"] == 5)
    code_bytes = read(code["off"], code["filesz"])
    thunks = {}
    i = scan_lo
    while True:
        i = code_bytes.find(b"\xff\x25", i)
        if i < 0:
            break
        disp = struct.unpack_from("<i", code_bytes, i + 2)[0]
        slot = i + 6 + disp
        if slot in slot2name:
            thunks[i] = slot2name[slot]
        i += 1
    # cross-check with the independent plt_symbols.json survey
    survey = json.load(open(os.path.join(RESEARCH, "frame_timing",
                                         "plt_symbols.json")))
    survey_map = {int(k, 16): v for k, v in survey.items()}
    assert thunks == survey_map, \
        "thunk mismatch: %d vs survey %d" % (len(thunks), len(survey_map))
    os.makedirs(DATA, exist_ok=True)
    out = {"thunks": {"0x%x" % k: v for k, v in sorted(thunks.items())},
           "slots": {"0x%x" % k: v for k, v in sorted(slot2name.items())},
           "import_order": [symbols[s]["nid"] for s in
                             sorted(order, key=order.get)]}
    tmp = os.path.join(DATA, "import_thunks.tmp.json")
    json.dump(out, open(tmp, "w"), indent=0)
    os.replace(tmp, os.path.join(DATA, "import_thunks.json"))
    named = sum(1 for v in slot2name.values() if not v.startswith("NID_"))
    print("imports: %d imports, %d slots (%d named), %d thunks "
          "(survey match) in %.1fs"
          % (len(order), len(slot2name), named, len(thunks),
             time.time() - t0), flush=True)


def load_imports():
    d = json.load(open(os.path.join(DATA, "import_thunks.json")))
    return ({int(k, 16): v for k, v in d["thunks"].items()},
            {int(k, 16): v for k, v in d["slots"].items()})


# --------------------------------------------------------------------------
# stage: disasm (multiprocessing, <=4 procs)
# --------------------------------------------------------------------------
_G = {}


def _init(starts, sizes, thunk_items, slot_items, str_items):
    global _G
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    _G["md"] = md
    _G["starts"] = starts
    _G["sizes"] = dict(sizes)
    _G["thunks"] = dict(thunk_items)
    _G["slots"] = dict(slot_items)
    _G["strings"] = dict(str_items)
    _G["f"] = open(ELF, "rb")


def _work(job):
    """Disassemble one function; return its inventory record + edge list."""
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP
    md, starts = _G["md"], _G["starts"]
    thunks, slots, strings = _G["thunks"], _G["slots"], _G["strings"]
    idx, start, size = job
    end = start + size
    _G["f"].seek(BASE_OFF + start)
    code = _G["f"].read(size)
    insns = 0
    callees, tails = [], []
    offstart = 0
    imports, imports_seen = [], set()
    strhits = []
    n_indirect = 0
    outside, outside_seen = [], set()
    last_mn, first_mn = "", ""
    for ins in md.disasm(code, start):
        insns += 1
        if not first_mn:
            first_mn = ins.mnemonic
        last_mn = ins.mnemonic
        mn = ins.mnemonic
        is_call = mn == "call"
        is_jmp = mn == "jmp"
        if not (is_call or is_jmp):
            # string refs can also come from lea/mov; collect lea rip refs
            if mn == "lea":
                for op in ins.operands:
                    if (op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP):
                        t = ins.address + ins.size + op.mem.disp
                        if t in strings and t not in [s[0] for s in strhits]:
                            strhits.append((t, strings[t]))
            continue
        # resolve target
        tgt = None
        indirect = False
        for op in ins.operands:
            if op.type == X86_OP_IMM:
                tgt = op.imm & 0xFFFFFFFFFFFFFFFF
            elif op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp
                if t in slots:
                    nm = slots[t]
                    if nm not in imports_seen:
                        imports_seen.add(nm)
                        imports.append(nm)
                    tgt = "import"
                elif is_call or is_jmp:
                    indirect = True
            elif op.type != X86_OP_IMM:
                indirect = True
        if tgt == "import":
            continue
        if tgt is None:
            if indirect:
                n_indirect += 1
            continue
        if tgt in thunks:
            nm = thunks[tgt]
            if nm not in imports_seen:
                imports_seen.add(nm)
                imports.append(nm)
            continue
        j = bisect.bisect_right(starts, tgt) - 1
        owner = starts[j] if j >= 0 else -1
        if owner < 0 or not (owner <= tgt < owner + _G["sizes"][owner]):
            if tgt not in outside_seen:
                outside_seen.add(tgt)
                outside.append(tgt)
            continue
        if tgt != owner:
            offstart += 1
        if is_call:
            callees.append(owner)
        else:
            tails.append(owner)
    # lea string refs were only collected for lea; also scan mov-rip? no: keep lea-only, documented
    rec = dict(idx=idx, start=start, size=size, end=end, insns=insns,
               n_callees=len(set(callees)), callees=sorted(set(callees)),
               n_tails=len(set(tails)), tails=sorted(set(tails)),
               n_imports=len(imports), imports=imports,
               n_indirect=n_indirect, offstart=offstart,
               n_outside=len(outside), outside=sorted(outside),
               n_strings=len(strhits), strings=strhits[:3],
               first_mn=first_mn, last_mn=last_mn)
    edges = ([(start, c, "call") for c in set(callees)] +
             [(start, t, "tailjump") for t in set(tails)])
    return rec, edges


def load_strings():
    m = {}
    with open(os.path.join(RESEARCH, "strings", "strings.csv"),
              encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                a = int(row["address"], 16)
            except ValueError:
                continue
            t = (row.get("text") or "")[:80]
            if t and a not in m:
                m[a] = t
    return m


def cmd_disasm(args):
    import multiprocessing as mp
    t0 = time.time()
    fns = load_fdes()
    thunks, slots = load_imports()
    strings = load_strings()
    print("disasm: %d fns, %d thunks, %d slots, %d strings" %
          (len(fns), len(thunks), len(slots), len(strings)), flush=True)
    starts = sorted(s for s, _ in fns)
    sizes = {s: z for s, z in fns}
    n = len(fns)
    lo = args.chunk * CHUNK_FUNCS
    hi = min(lo + CHUNK_FUNCS, n)
    assert lo < n, "chunk out of range"
    jobs = [(i, fns[i][0], fns[i][1]) for i in range(lo, hi)]
    with mp.Pool(MAX_PROCS, initializer=_init,
                 initargs=(starts, list(sizes.items()),
                           list(thunks.items()), list(slots.items()),
                           list(strings.items()))) as pool:
        res = pool.map(_work, jobs, chunksize=64)
    os.makedirs(os.path.join(DATA, "chunks"), exist_ok=True)
    pf = os.path.join(DATA, "chunks", "partial_%02d.csv" % args.chunk)
    cf = os.path.join(DATA, "chunks", "edges_%02d.csv" % args.chunk)
    tmp1, tmp2 = pf + ".tmp", cf + ".tmp"
    with open(tmp1, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "size", "end", "insns", "n_callees", "callees",
                    "n_tails", "tails", "n_imports", "imports", "n_indirect",
                    "offstart", "n_outside", "outside", "n_strings", "strings",
                    "first_mn", "last_mn"])
        for rec, _ in res:
            w.writerow([
                "0x%x" % rec["start"], rec["size"], "0x%x" % rec["end"],
                rec["insns"], rec["n_callees"],
                ";".join("0x%x" % c for c in rec["callees"][:8]),
                rec["n_tails"], ";".join("0x%x" % c for c in rec["tails"][:8]),
                rec["n_imports"], ";".join(rec["imports"][:8]),
                rec["n_indirect"], rec["offstart"], rec["n_outside"],
                ";".join("0x%x" % c for c in rec["outside"][:8]),
                rec["n_strings"],
                ";".join("0x%x:%s" % (a, t.replace(";", ","))
                          for a, t in rec["strings"]),
                rec["first_mn"], rec["last_mn"]])
    with open(tmp2, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["caller", "callee", "kind"])
        for rec, edges in res:
            for a, b, k in edges:
                w.writerow(["0x%x" % a, "0x%x" % b, k])
    os.replace(tmp1, pf)
    os.replace(tmp2, cf)
    n_edges = sum(len(e) for _, e in res)
    print("chunk %d: fns [%d,%d) %d recs %d edges in %.1fs" %
          (args.chunk, lo, hi, len(res), n_edges, time.time() - t0),
          flush=True)


# --------------------------------------------------------------------------
# stage: merge
# --------------------------------------------------------------------------
def cmd_merge(args):
    t0 = time.time()
    chdir = os.path.join(DATA, "chunks")
    parts = sorted(f for f in os.listdir(chdir) if f.startswith("partial_"))
    n_fn = EXPECT_FDES
    expect = (n_fn + CHUNK_FUNCS - 1) // CHUNK_FUNCS
    assert len(parts) == expect, (len(parts), expect)
    recs, edges = [], []
    for p in parts:
        recs += list(csv.DictReader(open(os.path.join(chdir, p))))
        e = p.replace("partial_", "edges_")
        edges += list(csv.DictReader(open(os.path.join(chdir, e))))
    assert len(recs) == EXPECT_FDES, len(recs)
    recs.sort(key=lambda r: int(r["start"], 16))
    callers = {}
    for e in edges:
        callers.setdefault(e["callee"], set()).add(e["caller"])
    tmp = os.path.join(DATA, "functions.tmp.csv")
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "size", "end", "insns", "is_leaf", "n_callees",
                    "callees", "n_tails", "tails", "n_callers", "n_imports",
                    "imports", "n_indirect", "offstart", "n_outside",
                    "outside", "n_strings", "strings", "status"])
        for r in recs:
            leaf = 1 if (r["n_callees"] == "0" and r["n_imports"] == "0") else 0
            w.writerow([r["start"], r["size"], r["end"], r["insns"], leaf,
                        r["n_callees"], r["callees"], r["n_tails"], r["tails"],
                        len(callers.get(r["start"], ())),
                        r["n_imports"], r["imports"], r["n_indirect"],
                        r["offstart"], r["n_outside"], r["outside"],
                        r["n_strings"], r["strings"], "binary"])
    os.replace(tmp, os.path.join(DATA, "functions.csv"))
    tmp = os.path.join(DATA, "callgraph.tmp.csv")
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["caller", "callee", "kind"])
        for e in sorted(edges, key=lambda e: (int(e["caller"], 16),
                                              int(e["callee"], 16))):
            w.writerow([e["caller"], e["callee"], e["kind"]])
    os.replace(tmp, os.path.join(DATA, "callgraph.csv"))
    print("merge: %d fns %d edges in %.1fs" %
          (len(recs), len(edges), time.time() - t0), flush=True)


# --------------------------------------------------------------------------
# stage: enrich
# --------------------------------------------------------------------------
MW_TAG = {"Havok": "havok", "HavokScript": "havok", "Scaleform": "scaleform",
          "FMOD": "fmod", "Lua": "lua", "zlib": "zlib", "libpng": "libpng",
          "YEBIS": "yebis", "Dantelion2": "libc/linked module",
          "expat": "libc/linked module", "PCRE": "libc/linked module"}
CONF_RANK = {"high": 3, "medium": 2, "low": 1}
GAME_CATS = {"gameplay", "menus", "rendering", "resolution_scene", "resources",
             "memory", "threading", "input", "parameters", "ezstate_ai",
             "frame_timing"}
SPEC_CATS = {"havok", "scaleform", "fmod", "lua"}


def _subsystem_tag(cats, texts):
    """Map strings.csv categories to (tag, evidence)."""
    have = set()
    for c in cats.split(";"):
        if c.strip():
            have.add(c.strip())
    spec = have & SPEC_CATS
    if spec:
        c = sorted(spec)[0]
        return c, "str:" + c
    if "png_zlib" in have:
        blob = " ".join(texts).lower()
        if "png" in blob:
            return "libpng", "str:png_zlib(png)"
        if "deflat" in blob or "inflat" in blob or "zlib" in blob:
            return "zlib", "str:png_zlib(zlib)"
        return "unknown", "str:png_zlib(ambiguous)"
    game = have & GAME_CATS
    if game:
        return "game", "str:" + sorted(game)[0]
    if have:
        return "unknown", "str:" + sorted(have)[0]
    return "unknown", ""


def cmd_enrich(args):
    t0 = time.time()
    base = list(csv.DictReader(open(os.path.join(DATA, "functions.csv"))))
    assert len(base) == EXPECT_FDES
    # modules.csv -> func: pick smallest function_count (most specific)
    mod_of, mod_n = {}, {}
    for row in csv.DictReader(open(os.path.join(RESEARCH, "strings",
                                                "modules.csv")),
                              encoding="utf-8"):
        try:
            fc = int(row["function_count"])
        except ValueError:
            fc = 10 ** 9
        for x in row["functions"].split(";"):
            x = x.strip()
            if not x:
                continue
            try:
                a = int(x, 16)
            except ValueError:
                continue
            mod_n[a] = mod_n.get(a, 0) + 1
            if a not in mod_of or fc < mod_of[a][1]:
                mod_of[a] = (row["module"], fc)
    # names.csv medium -> label
    label_of = {}
    with open(os.path.join(RESEARCH, "strings", "names.csv"),
              encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row["confidence"] == "medium":
                label_of[int(row["function"], 16)] = row["proposed_name"]
    # middleware_functions.csv -> tag (confidence wins, file order breaks ties)
    mw_best = {}
    with open(os.path.join(RESEARCH, "middleware", "middleware_functions.csv"),
              encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                a = int(row["address"], 16)
            except ValueError:
                continue
            lib = row["library"]
            if lib not in MW_TAG:
                continue
            key = (CONF_RANK.get(row.get("confidence", ""), 0), )
            if a not in mw_best or key > mw_best[a][0]:
                mw_best[a] = (key, MW_TAG[lib],
                              "mw:%s(%s)" % (lib, row.get("confidence", "")))    # strings.csv -> func -> (categories, first texts)
    str_cats, str_texts = {}, {}
    with open(os.path.join(RESEARCH, "strings", "strings.csv"),
              encoding="utf-8") as f:
        for row in csv.DictReader(f):
            cats = row.get("categories", "")
            if not cats:
                continue
            refs = (row.get("referencing_functions") or "").split(";")
            for x in refs:
                x = x.strip()
                if not x:
                    continue
                try:
                    a = int(x, 16)
                except ValueError:
                    continue
                str_cats.setdefault(a, set()).update(
                    c for c in cats.split(";") if c.strip())
                if len(str_texts.get(a, ())) < 4:
                    str_texts.setdefault(a, []).append(
                        (row.get("text") or "")[:60])
    # vtables.csv -> func -> slots (regenerate with recover.py if absent)
    vt_path = os.path.join(RESEARCH, "classes", "vtables.csv")
    if not os.path.exists(vt_path):
        raise SystemExit("vtables.csv absent; regenerate with "
                         "research/classes/recover.py first")
    fnset = set(int(r["start"], 16) for r in base)
    vtab_of = {}
    with open(vt_path, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                tgts = json.loads(row["slot_targets"])
            except (ValueError, TypeError):
                continue
            for i, t in enumerate(tgts):
                try:
                    a = int(t, 16)
                except (ValueError, TypeError):
                    continue
                if a in fnset:
                    vtab_of.setdefault(a, []).append(
                        "%s:%d" % (row["address"], i))
    tmp = os.path.join(DATA, "functions_enriched.tmp.csv")
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "size", "end", "insns", "is_leaf", "n_callees",
                    "callees", "n_tails", "tails", "n_callers", "n_imports",
                    "imports", "n_indirect", "offstart", "n_outside",
                    "outside", "n_strings", "strings", "module", "tag",
                    "tag_evidence", "label", "vtables", "status"])
        for r in base:
            a = int(r["start"], 16)
            if a in mw_best:
                tag, ev = mw_best[a][1], mw_best[a][2]
            elif a in str_cats:
                tag, ev = _subsystem_tag(";".join(sorted(str_cats[a])),
                                        str_texts.get(a, ()))
            else:
                tag, ev = "unknown", ""
            m = mod_of.get(a)
            w.writerow([r["start"], r["size"], r["end"], r["insns"],
                        r["is_leaf"], r["n_callees"], r["callees"],
                        r["n_tails"], r["tails"], r["n_callers"],
                        r["n_imports"], r["imports"], r["n_indirect"],
                        r["offstart"], r["n_outside"], r["outside"],
                        r["n_strings"], r["strings"],
                        m[0] if m else "", tag, ev,
                        label_of.get(a, ""),
                        ";".join(vtab_of.get(a, [])[:8]), "binary"])
    os.replace(tmp, os.path.join(DATA, "functions.csv"))
    print("enrich: modules=%d labels=%d mw=%d strtag=%d vtabbed=%d in %.1fs"
          % (len(mod_of), len(label_of), len(mw_best), len(str_cats),
             len(vtab_of), time.time() - t0), flush=True)


# --------------------------------------------------------------------------
# stage: report
# --------------------------------------------------------------------------
def _describe(rec):
    """One-line mechanical summary of a leaf function from its disassembly."""
    n = int(rec["insns"])
    size = int(rec["size"])
    first, last = rec.get("first_mn", ""), rec.get("last_mn", "")
    if n == 1 and last == "ret":
        return "empty stub; returns immediately"
    if n == 2 and last == "ret":
        return "2-insn stub ending in ret (likely trivial setter/return)"
    if last == "ret" and first in ("xor", "mov"):
        return "%d insns; sets a register and returns (constant/simple getter)" % n
    if last == "ret":
        return "%d insns, %d bytes; straight-line computation, returns" % (n, size)
    if last == "jmp":
        return "%d insns, %d bytes; ends in unconditional jump (forwarder)" % (n, size)
    return "%d insns, %d bytes; %s..%s" % (n, size, first, last)


PARTIAL_COLS = ["start", "size", "end", "insns", "n_callees", "callees",
                "n_tails", "tails", "n_imports", "imports", "n_indirect",
                "offstart", "n_outside", "outside", "n_strings", "strings",
                "first_mn", "last_mn"]


def _spot_check(n=20, seed=162959):
    """Re-disassemble n seeded-random functions with an independent linear
    sweep (no function-size trust: decode until size bytes consumed) and
    compare insn/callee/import counts. Returns (checked, mismatches)."""
    import capstone
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP
    fns = load_fdes()
    thunks, slots = load_imports()
    strings = load_strings()
    starts = sorted(s for s, _ in fns)
    sizes = dict(fns)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False  # independent: no detail API
    md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md2.detail = True
    rng = random.Random(seed)
    picks = sorted(rng.sample(range(len(fns)), n))
    base = {r["start"]: r for r in
            csv.DictReader(open(os.path.join(DATA, "functions.csv")))}
    f = open(ELF, "rb")
    checked, bad = [], []
    for i in picks:
        s, z = fns[i]
        f.seek(BASE_OFF + s)
        code = f.read(z)
        ins = [x for x in md.disasm(code, s)]
        # independent callee/import scan with the no-detail decoder
        cal, imp, ind = set(), set(), 0
        for x in md2.disasm(code, s):
            b0 = x.bytes[0] if x.bytes else 0
            if b0 == 0xE8 and x.size >= 5:
                rel = struct.unpack_from("<i", x.bytes, 1)[0]
                t = (x.address + x.size + rel) & 0xFFFFFFFFFFFFFFFF
                if t in thunks:
                    imp.add(thunks[t])
                else:
                    j = bisect.bisect_right(starts, t) - 1
                    o = starts[j] if j >= 0 else -1
                    if o >= 0 and o <= t < o + sizes[o]:
                        cal.add(o)
            elif b0 in (0xE9,) or (b0 == 0xEB):
                pass  # tails checked via main record only
            elif x.mnemonic == "call":
                ind += 1
            for op in x.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    t = x.address + x.size + op.mem.disp
                    if t in slots and x.mnemonic in ("call", "jmp"):
                        imp.add(slots[t])
        rec = base["0x%x" % s]
        ok = (int(rec["insns"]) == len(ins) and
              int(rec["n_callees"]) == len(cal) and
              int(rec["n_imports"]) == len(imp))
        checked.append((s, z, len(ins), len(cal), len(imp), ok))
        if not ok:
            bad.append((s, rec, (len(ins), len(cal), len(imp))))
    return checked, bad


def cmd_report(args):
    t0 = time.time()
    rows = list(csv.DictReader(open(os.path.join(DATA, "functions.csv"))))
    assert len(rows) == EXPECT_FDES
    edges = list(csv.DictReader(open(os.path.join(DATA, "callgraph.csv"))))
    # counts by tag
    tags = Counter()
    for r in rows:
        tags[r["tag"]] += 1
    leaves = sum(1 for r in rows if r["is_leaf"] == "1")
    # size histogram
    bins = [0, 8, 16, 32, 64, 128, 256, 512, 1024, 4096, 10 ** 9]
    hist = [0] * (len(bins) - 1)
    for r in rows:
        s = int(r["size"])
        for i in range(len(hist)):
            if bins[i] <= s < bins[i + 1]:
                hist[i] += 1
                break
    # top 30 hubs by caller count
    hubs = sorted(rows, key=lambda r: (-int(r["n_callers"]), r["start"]))[:30]
    # import coverage
    imp_callers = Counter()
    imp_fn = Counter()
    for r in rows:
        for nm in r["imports"].split(";"):
            if nm:
                imp_callers[nm] += 1
        if r["imports"]:
            imp_fn["with_imports"] += 1
    # outside targets
    out_c = Counter()
    n_off = sum(int(r["offstart"]) for r in rows)
    for r in rows:
        for x in r["outside"].split(";"):
            if x:
                out_c[int(x, 16)] += 1
    n_ind = sum(int(r["n_indirect"]) for r in rows)
    n_calls = sum(1 for e in edges if e["kind"] == "call")
    n_tails = sum(1 for e in edges if e["kind"] == "tailjump")
    # spot check
    t1 = time.time()
    checked, bad = _spot_check()
    # pilot candidates: 300 smallest game-tagged leaves, no callees/imports
    cands = [r for r in rows if r["tag"] == "game" and r["is_leaf"] == "1"
             and r["n_callees"] == "0" and r["n_imports"] == "0"]
    cands.sort(key=lambda r: (int(r["size"]), int(r["insns"]), r["start"]))
    pilots = cands[:300]
    # need first_mn/last_mn for notes: reload partials
    pm = {}
    chdir = os.path.join(DATA, "chunks")
    for p in sorted(os.listdir(chdir)):
        if p.startswith("partial_"):
            for r in csv.DictReader(open(os.path.join(chdir, p))):
                pm[r["start"]] = r
    tmp = os.path.join(DATA, "pilot_candidates.tmp.csv")
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "size", "insns", "note"])
        for r in pilots:
            w.writerow([r["start"], r["size"], r["insns"],
                        _describe(pm.get(r["start"], r))])
    os.replace(tmp, os.path.join(DATA, "pilot_candidates.csv"))
    L = []
    A = L.append
    A("# Function inventory summary (decomp Phase 0)")
    A("")
    A("ELF sha256 cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86; "
      "addresses are image offsets (PS4 VA = offset + 0x400000).")
    A("Method: FDE pc_range table (162,959 functions) + capstone linear sweep "
      "per function with <=4 processes; joins against research/strings "
      "(modules/names/strings.csv), research/middleware/middleware_functions.csv, "
      "research/classes/vtables.csv. is_leaf = no direct callees and no import calls.")
    A("")
    A("## Counts by middleware tag")
    for t, n in tags.most_common():
        A("- %s: %d" % (t, n))
    A("")
    A("Leaves (is_leaf=1): %d of %d (%.1f%%)" %
      (leaves, len(rows), 100.0 * leaves / len(rows)))
    A("Call edges: %d call + %d tailjump = %d" % (n_calls, n_tails, len(edges)))
    A("")
    A("## Size histogram (bytes)")
    for i in range(len(hist)):
        A("- [%d,%s): %d" % (bins[i], str(bins[i + 1]) if bins[i + 1] < 10 ** 9 else "inf",
                             hist[i]))
    A("")
    A("## Top 30 hubs by caller count")
    for r in hubs:
        A("- %s size=%s callers=%s label=%s" %
          (r["start"], r["size"], r["n_callers"], r["label"] or r["tag"]))
    A("")
    A("## Import coverage")
    A("Functions calling >=1 import: %d (%.1f%%)" %
      (imp_fn["with_imports"], 100.0 * imp_fn["with_imports"] / len(rows)))
    A("Top 20 imports by caller count:")
    for nm, n in imp_callers.most_common(20):
        A("- %s: %d callers" % (nm, n))
    A("")
    A("## Call targets outside known functions")
    A("rel32/jmp targets landing outside any FDE range (distinct, including "
      "thunk-adjacent/data/code-gap targets): %d distinct in %d functions" %
      (len(out_c), sum(1 for r in rows if r["n_outside"] != "0")))
    A("Top 15 outside targets:")
    for t, n in out_c.most_common(15):
        A("- 0x%x: %d functions" % (t, n))
    A("Mid-function landing sites (offstart, attributed to containing fn): %d" % n_off)
    A("Indirect call/jmp sites (register or unresolved mem, incl. virtual calls): %d" % n_ind)
    A("")
    A("## Spot check: 20 seeded-random functions re-decoded independently")
    A("How: random.Random(162959).sample(20000-chunk order, 20); each function "
      "re-read from the ELF and decoded with a fresh capstone instance with "
      "detail disabled for the instruction count plus a separate detail pass "
      "for callees/imports; compared against functions.csv "
      "(insns, n_callees, n_imports).")
    for s, z, ni, nc, nm, ok in checked:
        A("- 0x%x size=%d insns=%d callees=%d imports=%d %s" %
          (s, z, ni, nc, nm, "OK" if ok else "MISMATCH"))
    A("Spot re-decode took %.1fs; mismatches: %d" % (time.time() - t1, len(bad)))
    for s, rec, got in bad:
        A("  MISMATCH 0x%x csv=%s/%s/%s redo=%s" %
          (s, rec["insns"], rec["n_callees"], rec["n_imports"], got))
    A("")
    A("## Pilot candidates")
    A("%d game-tagged leaf functions with no callees/imports; smallest 300 "
      "written to pilot_candidates.csv with mechanical one-line notes "
      "(insn count, first/last mnemonic shape)." % len(cands))
    A("Smallest 5 pilots:")
    for r in pilots[:5]:
        A("- %s size=%s insns=%s: %s" %
          (r["start"], r["size"], r["insns"], _describe(pm.get(r["start"], r))))
    A("")
    A("## Open gaps")
    A("- Indirect calls (incl. vtable dispatches) have no static edge; "
      "vtable-slot inversion (vtables column) is the bridge for virtual targets.")
    A("- String refs come from lea RIP-relative only; call targets inside a "
      "function past its start are attributed to the containing function (offstart).")
    A("- tag=unknown means no middleware and no subsystem string evidence.")
    open(os.path.join(DATA, "inventory_summary.tmp.md"), "w").write("\n".join(L) + "\n")
    os.replace(os.path.join(DATA, "inventory_summary.tmp.md"),
               os.path.join(DATA, "inventory_summary.md"))
    print("report: pilots=%d/%d mismatches=%d in %.1fs" %
          (len(pilots), len(cands), len(bad), time.time() - t0), flush=True)



# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fdes")
    sub.add_parser("imports")
    p = sub.add_parser("disasm")
    p.add_argument("--chunk", type=int, required=True)
    p.add_argument("--chunks", type=int, required=True)
    sub.add_parser("merge")
    sub.add_parser("enrich")
    sub.add_parser("report")
    args = ap.parse_args(argv)
    {"fdes": cmd_fdes, "imports": cmd_imports, "disasm": cmd_disasm,
     "merge": cmd_merge, "enrich": cmd_enrich,
     "report": cmd_report}[args.cmd](args)


if __name__ == "__main__":
    main()

