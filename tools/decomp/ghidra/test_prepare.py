"""Synthetic fixture tests; no proprietary bytes or dependency on game files."""
import csv
import json
from pathlib import Path
import struct
import tempfile
import unittest
from prepare import extract


class PrepareTests(unittest.TestCase):
    def fixture(self, root):
        b = bytearray(2048)
        b[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<Q', b, 32, 64)
        struct.pack_into('<HH', b, 54, 56, 4)
        headers = [(1, 5, 512, 0, 0, 256, 256, 16),
                    (0x6474e550, 4, 768, 128, 0, 28, 28, 4),
                    (2, 6, 800, 0, 0, 112, 112, 8),
                    (0x61000000, 4, 1024, 0, 0, 400, 0, 16)]
        for i, h in enumerate(headers):
            struct.pack_into('<IIQQQQQQ', b, 64+56*i, *h)
        b[768:772] = bytes([1, 0x1b, 3, 0x3b])
        struct.pack_into('<Iiiii', b, 776, 2, -112, 0, -96, 0)
        tags = [(0x61000035, 0), (0x61000037, 16), (0x61000039, 32),
                (0x6100003f, 48), (0x61000029, 128), (0x6100002d, 48),
                (0x6100002f, 200), (0x61000031, 0)]
        # Expand the declared dynamic size to include all eight tags.
        struct.pack_into('<Q', b, 64+56*2+32, 128)
        for i, pair in enumerate(tags):
            struct.pack_into('<QQ', b, 800+16*i, *pair)
        b[1024:1032] = b'\0abc#x#y'
        struct.pack_into('<IBBHQQ', b, 1024+32+24, 1, 2, 0, 0, 0, 0)
        struct.pack_into('<QQq', b, 1152, 64, 8, 32)
        struct.pack_into('<QQq', b, 1176, 80, (1<<32)|7, 0)
        source = root/'fixture.elf'; source.write_bytes(b)
        names = root/'names.inc'; names.write_text('{"abc#x#y","test_import"}')
        link = root/'link.json'; link.write_text(json.dumps({'imports':1}))
        guessed = root/'names.csv'
        guessed.write_text('function,size,proposed_name,confidence,kind\n'
                           '0x10,28,Alpha,medium,scoped method diagnostic\n'
                           '0x20,28,Beta,low,subsystem association\n')
        return source, names, link, guessed

    def test_extract_preserves_input_and_resolves_slots(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = self.fixture(Path(tmp)); before = args[0].read_bytes()
            b, manifest = extract(args[0], args[1], args[2], args[3])
            self.assertEqual(before, args[0].read_bytes())
            self.assertEqual(manifest['starts'], ['0x10', '0x20'])
            self.assertEqual(manifest['imports'], [{'address':'0x50', 'nid':'abc#x#y', 'name':'test_import'}])
            # Only medium-confidence names are applied, with a guess_ prefix.
            self.assertEqual(manifest['guessed_names'], {'0x10': 'guess_Alpha'})
            self.assertEqual(struct.unpack_from('<Q', b, 576)[0], 32)
            self.assertEqual(struct.unpack_from('<H', b, 16)[0], 2)
            self.assertEqual(struct.unpack_from('<I', b, 64+56*2)[0], 0)

    def test_bad_encoding_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = self.fixture(Path(tmp)); b = bytearray(args[0].read_bytes())
            b[771] = 0; args[0].write_bytes(b)
            with self.assertRaisesRegex(ValueError, 'encoding'):
                extract(args[0], args[1], args[2], args[3])


if __name__ == '__main__':
    unittest.main()
