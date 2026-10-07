import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
from bbport_assets import dll_model_dir, dll_model_problem, dll_model_version, sdk_model_dir


class UpscalerAssetsTests(unittest.TestCase):
    def test_tier_depends_on_output_not_render_preset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertIn('t2160_m0/spd.spv', dll_model_problem(root, '3840x2160', 3))
            self.assertIn('t2160_m0/spd.spv', dll_model_problem(root, '2560x1440', 0))
            self.assertIn('t1080_m1/spd.spv', dll_model_problem(root, '1280x720', 4))

    def test_partial_model_is_not_reported_available(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / 't2160_m0'
            folder.mkdir()
            (folder / 'spd.spv').write_bytes(bytes(20))
            self.assertIn('prepass.spv', dll_model_problem(root, '3840x2160', 0))

    def test_complete_model_and_wrong_initializer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / 't1080_m0'
            folder.mkdir()
            names = ['spd', 'prepass', 'pass0_post', 'postpass', 'rcas']
            for i in range(1, 13):
                names.extend([f'pass{i}', f'pass{i}_post'])
            for name in names:
                (folder / (name+'.spv')).write_bytes(bytes(20))
            (folder / 'initializer.bin').write_bytes(bytes(8))
            self.assertIn('initializer.bin', dll_model_problem(root, '1920x1080', 1))
            (folder / 'initializer.bin').write_bytes(bytes(131072))
            self.assertIsNone(dll_model_problem(root, '1920x1080', 1))

    def test_folder_resolution_follows_run_sh(self):
        with tempfile.TemporaryDirectory() as tmp:
            port, data = Path(tmp) / 'port', Path(tmp) / 'data'
            port.mkdir()
            data.mkdir()
            self.assertEqual(dll_model_dir(port, data, {}), data / 'fsr4_dll')
            (data / 'fsr4_411').mkdir()  # the folder before fsr4_model
            self.assertEqual(dll_model_dir(port, data, {}), data / 'fsr4_411')
            (port / 'fsr4_dll').mkdir()
            self.assertEqual(dll_model_dir(port, data, {}), port / 'fsr4_dll')
            self.assertEqual(dll_model_dir(port, data, {'BB_FSR411_DIR': '/a'}), Path('/a'))
            self.assertEqual(dll_model_dir(port, data, {'BB_FSR4_DLL_DIR': '/b', 'BB_FSR411_DIR': '/a'}),
                             Path('/b'))

    def test_bundled_model_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            port, data = Path(tmp) / 'port', Path(tmp) / 'data'
            port.mkdir()
            self.assertEqual(sdk_model_dir(port, data, {}), data / 'fsr4_shaders')
            (port / 'fsr4_shaders').mkdir()
            self.assertEqual(sdk_model_dir(port, data, {}), port / 'fsr4_shaders')
            self.assertEqual(sdk_model_dir(port, data, {'BB_FSR4_DIR': '/c'}), Path('/c'))

    def test_version_comes_from_the_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertIsNone(dll_model_version(root))
            (root / 'manifest.json').write_text('{"layout": "fsr4cap-1", "upscaler_version": "4.1.1.3529"}')
            self.assertEqual(dll_model_version(root), '4.1.1.3529')
            (root / 'manifest.json').write_text('not json')
            self.assertIsNone(dll_model_version(root))
