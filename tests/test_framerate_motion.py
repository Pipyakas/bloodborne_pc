import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('framerate_motion',
    Path(__file__).resolve().parents[1] / 'tools' / 'test_framerate_motion.py')
motion = importlib.util.module_from_spec(spec)
spec.loader.exec_module(motion)


class MotionSummaryTests(unittest.TestCase):
    def test_uses_wall_time_not_requested_frame_cap(self):
        log = ('Camera trace: frame=10 ns=1000000000 xyz=1,2,3\n'
               'Camera trace: frame=70 ns=2000000000 xyz=4,6,3\n')
        result = motion.motion_summary(log, 0, 3_000_000_000)
        self.assertEqual(result['observed_scene_fps'], 60)
        self.assertEqual(result['camera_speed'], 5)
        self.assertEqual(result['camera_path_speed'], 5)

    def test_excludes_samples_outside_input_interval(self):
        log = ''.join(f'Camera trace: frame={i} ns={i * 1000000000} xyz={i},0,0\n'
                      for i in range(4))
        result = motion.motion_summary(log, 1_000_000_000, 2_000_000_000)
        self.assertEqual(result['samples'], 2)
        self.assertEqual(result['camera_speed'], 1)

    def test_rejects_missing_or_invalid_telemetry(self):
        with self.assertRaises(ValueError):
            motion.motion_summary('Camera trace: frame=1 ns=1 xyz=nan,0,0\n', 0, 2)
