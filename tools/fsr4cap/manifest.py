#!/usr/bin/env python3
# Provenance manifest for the "FSR 4 DLL model" asset set built by extract.py: which upscaler DLL
# (and loader, if any) the model weights came from, its file version, and the model sets present.
# The replay's shaders and weights are translated from that DLL (extract.py); the user builds them.
#
#   manifest.py <assets dir> <upscaler dll> [<loader dll>] [--no-verify]
#
# With verification on, every <set>/initializer.bin must occur verbatim inside the upscaler DLL:
# it is the blob the DLL uploads, so a set that is not in the DLL is not a FSR 4 DLL model.
import json
import os
import struct
import sys

LAYOUT = 'fsr4cap-1'  # pass layout understood by the replay runtime (extract.py names its passes)
FORMAT = 1
SIGNATURE = 0xFEEF04BD  # VS_FIXEDFILEINFO.dwSignature
SETS = ('t1080_m0', 't1080_m1', 't2160_m0', 't2160_m1')
SECTION_HEADER_SIZE = 40


def _rsrc_range(data):
    """Return (offset, size) of the .rsrc section's raw data, or None if data is not a PE with one."""
    if data[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return None
    sections, opt_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    for i in range(sections):
        off = pe + 24 + opt_size + i * SECTION_HEADER_SIZE
        name, _, _, size, offset = struct.unpack_from('<8sIIII', data, off)
        if name.rstrip(b'\0') == b'.rsrc':
            return offset, size
    return None


def _fixed_file_info(data):
    """Yield VS_FIXEDFILEINFO candidate offsets inside the .rsrc raw data.

    The resource directory is a tree of structures, so the signature is only searched for at
    4-byte aligned offsets; the first hit in file order wins, as the resource directory is sorted.
    """
    rsrc = _rsrc_range(data)
    if rsrc is None:
        return
    raw, raw_size = rsrc
    limit = min(raw_size, len(data) - raw)
    for off in range(0, limit - 4, 4):
        if struct.unpack_from('<I', data, raw + off)[0] == SIGNATURE:
            yield raw + off


def pe_file_version(path):
    """Return the PE file version of path as "a.b.c.d", or None if it is not a PE or has no version."""
    data = open(path, 'rb').read()
    for off in _fixed_file_info(data):
        ms, ls = struct.unpack_from('<II', data, off + 8)  # dwFileVersionMS/LS follow the header
        return f'{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}'
    return None


def initializer_in_dll(dll_bytes, init_bytes):
    """True if the model initializer blob occurs verbatim in the DLL bytes."""
    return init_bytes in dll_bytes


def found_sets(assets_dir):
    """Sorted names of the model sets under assets_dir that carry an initializer."""
    return sorted(name for name in SETS if os.path.isfile(os.path.join(assets_dir, name, 'initializer.bin')))


def write_manifest(assets_dir, upscaler_dll, loader_dll=None, verify=True):
    """Write assets_dir/manifest.json describing the DLL model set there and return it."""
    dll = open(upscaler_dll, 'rb').read()
    sets = found_sets(assets_dir)
    if not sets:
        raise ValueError(f'no initializer.bin in any of {", ".join(SETS)} under {assets_dir}')
    if verify:
        for name in sets:
            init = open(os.path.join(assets_dir, name, 'initializer.bin'), 'rb').read()
            if not initializer_in_dll(dll, init):
                raise ValueError(f'{name}/initializer.bin not found in {os.path.basename(upscaler_dll)}')
    manifest = {
        'format': FORMAT,
        'layout': LAYOUT,
        'upscaler_dll': os.path.basename(upscaler_dll),
        'upscaler_version': pe_file_version(upscaler_dll),
        'loader_version': pe_file_version(loader_dll) if loader_dll else None,
        'sets': sets,
    }
    with open(os.path.join(assets_dir, 'manifest.json'), 'w') as out:
        json.dump(manifest, out, indent=2)
        out.write('\n')
    return manifest


def read_manifest(assets_dir):
    """Return the manifest of assets_dir, or None if it has none."""
    path = os.path.join(assets_dir, 'manifest.json')
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return json.load(f)


def main(argv):
    args = [a for a in argv if a != '--no-verify']
    verify = '--no-verify' not in argv
    if not 2 <= len(args) <= 3:
        print('usage: manifest.py <assets dir> <upscaler dll> [<loader dll>] [--no-verify]', file=sys.stderr)
        return 2
    try:
        manifest = write_manifest(args[0], args[1], args[2] if len(args) == 3 else None, verify=verify)
    except ValueError as e:
        print(f'manifest: {e}', file=sys.stderr)
        return 1
    version = manifest['upscaler_version'] or 'unknown version'
    print(f'manifest: FSR 4 DLL model {version} (layout {manifest["layout"]}), sets {" ".join(manifest["sets"])}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))