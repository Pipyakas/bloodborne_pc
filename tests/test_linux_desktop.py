#!/usr/bin/env python3
"""Native desktop launcher tests without a game, GPU, or desktop session."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinuxDesktopTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='bbport-desktop-')
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        tools = self.repo / 'tools/cross'
        tools.mkdir(parents=True)
        shutil.copy(ROOT / 'tools/cross/launch-linux.sh', tools)
        self.dist = self.repo / 'dist/linux'
        (self.dist / 'out').mkdir(parents=True)
        probe = self.dist / 'out/bb-probe'
        probe.touch()
        probe.chmod(0o755)
        (self.dist / 'BUILD_BACKEND').write_text('native\n')
        (self.dist / 'run.sh').write_text(
            'printf "%s\\n" "$BB_PROBE" "$BB_DATA_DIR" "$BB_PREBUILT" "$@"\n')
        game = self.repo / 'game with spaces'
        game.mkdir()
        (game / 'eboot.bin').touch()
        self.env = dict(os.environ, BB_GAME_DIR=str(game),
                        BB_DATA_DIR=str(self.repo / 'persistent data'),
                        XDG_CONFIG_HOME=str(self.repo / 'config'),
                        PATH='/usr/bin:/bin')

    def launch(self, *args):
        return subprocess.run(['bash', str(self.repo / 'tools/cross/launch-linux.sh'), *args],
                              env=self.env, capture_output=True, text=True)

    def test_latest_build_and_persistent_data(self):
        result = self.launch('--example')
        self.assertEqual(result.returncode, 0, result.stderr)
        log = Path(self.env['BB_DATA_DIR']) / 'out/desktop-launch.log'
        self.assertEqual(log.read_text().splitlines(),
                         [str(self.dist / 'out/bb-probe'), self.env['BB_DATA_DIR'], '1', '--example'])

    def test_reject_container_build(self):
        (self.dist / 'BUILD_BACKEND').write_text('container\n')
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('container build', result.stderr)

    def test_missing_game(self):
        self.env['BB_GAME_DIR'] = str(self.repo / 'missing')
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('BB_GAME_DIR', result.stderr)

    def test_missing_binary(self):
        (self.dist / 'out/bb-probe').unlink()
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No Linux build', result.stderr)


if __name__ == '__main__':
    unittest.main()
