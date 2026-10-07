import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ROOT

sys.path.insert(0, str(ROOT / 'tools' / 'fsr4cap'))
import manifest

WITCHER_UPSCALER = Path('/var/mnt/hdd/SteamLibrary/steamapps/common/The Witcher 3/bin/x64_dx12'
                         '/amd_fidelityfx_upscaler_dx12.dll')
VERSION = (4, 1, 1, 2740)
INITIALIZER_SIZE = 131072


def build_pe(version=VERSION, initializer=None, sections=('.text', '.rsrc'), valid=True,
             section_size=0x200):
    """Build a minimal PE: DOS stub, PE32+ header, .text with an optional initializer blob,
    and a .rsrc whose raw data holds a VS_FIXEDFILEINFO at a 4-byte aligned offset.

    version is (major, minor, build, revision); section_size truncates .rsrc (no version
    resource); valid=False corrupts the PE signature.
    """
    pe_off, opt_size, file_align, header_size = 0x80, 240, 0x200, 0x200
    text = b'\xcc' * file_align
    if initializer is not None:
        text = text[:64] + initializer + text[64 + len(initializer):]

    ms, ls = (version[0] << 16) | version[1], (version[2] << 16) | version[3]
    fixed = struct.pack('<IIII', manifest.SIGNATURE, 0x00010000, ms, ls)  # then dwProductVersion etc.
    fixed += b'\0' * (52 - len(fixed))  # the rest of the 52-byte structure is irrelevant here
    padding = b'\0' * 12  # keeps the VS_FIXEDFILEINFO 4-byte aligned inside .rsrc
    rsrc = ((b'\0' * (file_align - len(padding) - len(fixed))) + padding + fixed)[:section_size]

    payloads = [(name, rsrc if name == '.rsrc' else text) for name in sections]
    sections_blob, offset, data = b'', header_size, b''
    for name, payload in payloads:
        sections_blob += struct.pack('<8sIIIIIIHHI', name.encode(), len(payload), header_size,
                                     len(payload), offset, 0, 0, 0, 0, 0)
        offset, data = offset + len(payload), data + payload

    image = bytearray(b'\0' * header_size)
    image[0:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, pe_off)
    image[pe_off:pe_off + 4] = b'PE\0\0' if valid else b'XX\0\0'
    image[pe_off + 4:pe_off + 24] = struct.pack('<HHIIIHH', 0x8664, len(sections), 0, 0, 0, opt_size, 0)
    opt = struct.pack('<HBBII', 0x20b, 14, 0, file_align, file_align)
    opt += b'\0' * (opt_size - len(opt))  # the rest of the optional header may be anything
    image[pe_off + 24:pe_off + 24 + opt_size] = opt
    table_at = pe_off + 24 + opt_size
    assert table_at + len(sections_blob) <= header_size, 'section table must fit in the headers'
    image[table_at:table_at + len(sections_blob)] = sections_blob
    return bytes(image) + data


def set_up(assets, name, initializer, shaders=('spd.spv',)):
    folder = Path(assets) / name
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'initializer.bin').write_bytes(initializer)
    for shader in shaders:
        (folder / shader).write_bytes(b'\x03\x02\x23\x07')


class PeFileVersionTests(unittest.TestCase):
    def test_version_from_rsrc(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'upscaler.dll'
            path.write_bytes(build_pe(initializer=bytes(64)))
            self.assertEqual(manifest.pe_file_version(path), '4.1.1.2740')

    def test_version_without_text_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'rsrc-only.dll'
            path.write_bytes(build_pe(sections=('.rsrc',)))
            self.assertEqual(manifest.pe_file_version(path), '4.1.1.2740')

    def test_missing_version_and_non_pe(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad_sig, truncated, no_rsrc, not_pe = (Path(tmp) / n for n in ('a.dll', 'b.dll', 'c.dll', 'd.dll'))
            bad_sig.write_bytes(build_pe(valid=False))
            truncated.write_bytes(build_pe(section_size=8))
            no_rsrc.write_bytes(build_pe(sections=('.text',)))
            not_pe.write_bytes(b'dll\x00' + bytes(4096))
            for path in (bad_sig, truncated, no_rsrc, not_pe):
                self.assertIsNone(manifest.pe_file_version(path), path.name)


class InitializerSearchTests(unittest.TestCase):
    def test_finds_embedded_blob(self):
        blob = bytes(range(256)) * 512
        self.assertTrue(manifest.initializer_in_dll(build_pe(initializer=blob), blob))

    def test_absent_blob(self):
        blob = bytes(range(256)) * 512
        self.assertFalse(manifest.initializer_in_dll(build_pe(), blob))


class WriteManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.assets = Path(self.tmp.name) / 'fsr4_assets'
        self.assets.mkdir()
        self.t1080, self.t2160 = b'\x11' * INITIALIZER_SIZE, b'\x22' * INITIALIZER_SIZE
        set_up(self.assets, 't1080_m0', self.t1080)
        set_up(self.assets, 't2160_m0', self.t2160)
        self.dll = Path(self.tmp.name) / 'amd_fidelityfx_upscaler_dx12.dll'
        self.dll.write_bytes(build_pe(initializer=self.t1080 + bytes(1024) + self.t2160))
        self.addCleanup(self.tmp.cleanup)

    def test_manifest_fields(self):
        result = manifest.write_manifest(self.assets, self.dll)
        self.assertEqual(result, manifest.read_manifest(self.assets))
        self.assertEqual(result, {'format': 1, 'layout': manifest.LAYOUT,
                                  'upscaler_dll': 'amd_fidelityfx_upscaler_dx12.dll',
                                  'upscaler_version': '4.1.1.2740', 'loader_version': None,
                                  'sets': ['t1080_m0', 't2160_m0']})
        self.assertEqual(manifest.LAYOUT, 'fsr4cap-1')

    def test_written_json_is_indented_with_trailing_newline(self):
        manifest.write_manifest(self.assets, self.dll)
        text = (self.assets / 'manifest.json').read_text()
        self.assertTrue(text.endswith('}\n'))
        self.assertIn('\n  "layout": "fsr4cap-1",', text)
        self.assertEqual(json.loads(text)['sets'], ['t1080_m0', 't2160_m0'])

    def test_loader_version(self):
        loader = Path(self.tmp.name) / 'amd_fidelityfx_vk.dll'
        loader.write_bytes(build_pe(version=(1, 2, 3, 4)))
        self.assertEqual(manifest.write_manifest(self.assets, self.dll, loader)['loader_version'], '1.2.3.4')

    def test_null_version_without_resource_version(self):
        self.dll.write_bytes(build_pe(section_size=8))
        self.assertIsNone(manifest.write_manifest(self.assets, self.dll, verify=False)['upscaler_version'])

    def test_foreign_initializer_is_rejected(self):
        set_up(self.assets, 't1080_m1', b'\x33' * INITIALIZER_SIZE)
        with self.assertRaises(ValueError) as e:
            manifest.write_manifest(self.assets, self.dll)
        self.assertIn('t1080_m1', str(e.exception))
        self.assertFalse((self.assets / 'manifest.json').exists())

    def test_verify_false_skips_the_check(self):
        set_up(self.assets, 't1080_m1', b'\x33' * INITIALIZER_SIZE)
        result = manifest.write_manifest(self.assets, self.dll, verify=False)
        self.assertEqual(result['sets'], ['t1080_m0', 't1080_m1', 't2160_m0'])

    def test_only_sets_with_an_initializer_are_listed(self):
        (self.assets / 't2160_m1').mkdir()
        (self.assets / 't2160_m1' / 'spd.spv').write_bytes(b'\x03\x02\x23\x07')
        self.assertEqual(manifest.write_manifest(self.assets, self.dll)['sets'], ['t1080_m0', 't2160_m0'])

    def test_no_sets_is_an_error(self):
        empty = Path(self.tmp.name) / 'empty'
        empty.mkdir()
        with self.assertRaises(ValueError):
            manifest.write_manifest(empty, self.dll)

    def test_missing_manifest_reads_as_none(self):
        self.assertIsNone(manifest.read_manifest(self.assets))
        manifest.write_manifest(self.assets, self.dll)
        self.assertIsNotNone(manifest.read_manifest(self.assets))


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.assets = Path(self.tmp.name) / 'assets'
        self.assets.mkdir()
        self.init = b'\x44' * INITIALIZER_SIZE
        set_up(self.assets, 't1080_m0', self.init, shaders=('spd.spv', 'postpass.spv'))
        self.dll = Path(self.tmp.name) / 'amd_fidelityfx_upscaler_dx12.dll'
        self.dll.write_bytes(build_pe(initializer=self.init))
        self.addCleanup(self.tmp.cleanup)

    def test_prints_one_summary_line(self):
        code = manifest.main([str(self.assets), str(self.dll)])
        self.assertEqual(code, 0)
        self.assertEqual(manifest.read_manifest(self.assets)['sets'], ['t1080_m0'])

    def test_no_verify_flag(self):
        (self.assets / 't1080_m0' / 'initializer.bin').write_bytes(b'\x55' * INITIALIZER_SIZE)
        self.assertEqual(manifest.main([str(self.assets), str(self.dll), '--no-verify']), 0)
        self.assertEqual(manifest.main([str(self.assets), str(self.dll)]), 1)


@unittest.skipUnless(WITCHER_UPSCALER.is_file(), f'{WITCHER_UPSCALER} not installed')
class InstalledUpscalerTests(unittest.TestCase):
    def test_file_version(self):
        self.assertEqual(manifest.pe_file_version(WITCHER_UPSCALER), '4.1.1.2740')


if __name__ == '__main__':
    unittest.main()