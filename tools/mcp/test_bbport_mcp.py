#!/usr/bin/env python3
"""Launch-mode regression tests; no game or desktop access required."""
import unittest
from unittest.mock import MagicMock, patch

import bbport_mcp as mcp


class LaunchModeTests(unittest.TestCase):
    def launch_environment(self, **kwargs):
        game = mcp.Game()
        process = MagicMock()
        process.poll.return_value = None
        with patch.dict(mcp.os.environ, {'BB_HIDDEN': '1', 'BB_MINIMIZED': '1'}), \
             patch.object(mcp.subprocess, 'Popen', return_value=process) as popen, \
             patch.object(mcp, 'kill_on_close_job'), \
             patch.object(mcp, 'read_log', return_value='Runtime: control on 127.0.0.1:12345'), \
             patch.object(game, 'connect'), \
             patch('builtins.open', unittest.mock.mock_open()), \
             patch.object(mcp.Path, 'mkdir'):
            game.launch(**kwargs)
        return popen.call_args.kwargs['env']

    def test_default_is_minimized_and_silent(self):
        env = self.launch_environment()
        self.assertEqual((env['BB_HIDDEN'], env['BB_MINIMIZED'], env['BB_AUDIO']), ('0', '1', 'none'))

    def test_hidden_overrides_default_minimized(self):
        env = self.launch_environment(hidden=True)
        self.assertEqual((env['BB_HIDDEN'], env['BB_MINIMIZED'], env['BB_AUDIO']), ('1', '0', 'none'))

    def test_visible_clears_inherited_modes(self):
        env = self.launch_environment(hidden=False, minimized=False)
        self.assertEqual((env['BB_HIDDEN'], env['BB_MINIMIZED']), ('0', '0'))

    def test_minimized_audio_opt_in(self):
        self.assertEqual(self.launch_environment(minimized=True, audio=True)['BB_AUDIO'], 'sdl')

    def test_tool_forwards_minimized(self):
        with patch.object(mcp.GAME, 'launch') as launch, \
             patch.object(mcp.GAME, 'status', return_value={'pad_open': 1}), \
             patch.object(mcp, 'tail', return_value=''):
            mcp.game_launch()
        self.assertTrue(launch.call_args.kwargs['minimized'])

    def test_tool_schema_defaults(self):
        props = mcp.TOOLS['game_launch']['schema']['inputSchema']['properties']
        self.assertTrue(props['minimized']['default'])
        self.assertFalse(props['hidden']['default'])


if __name__ == '__main__':
    unittest.main()
