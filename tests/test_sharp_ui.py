"""Sharp UI (bbport.ini sharp_ui, from Awaken's Lantern Engine): the game's UI viewport at the output's
size (patches.py ui_viewport), written into the game's resolution patch, the scene kept clear of the
UI's sizes (scaled_sizes) and passed to the port as BB_UI_RES (run.py)."""
from paths import ROOT
import unittest

from patches import (EBOOT_BASE, OUTPUT_SIZE, RESOLUTION_TEMPLATE, UI_HEIGHT, UI_WIDTH, compile_patches, hud_canvas,
                     resolution_writes, scaled_sizes, ui_viewport)

XML = ROOT / 'patches/Bloodborne.xml'
SEGMENTS = [(0, 0x6000000)]
SHARP = {'sharp_ui': '1'}


class UiViewportTests(unittest.TestCase):
    def test_off_by_default_the_canvas_as_before(self):
        for output in [(2560, 1440), (3840, 2160), (2560, 1600), (3440, 1440), (1280, 720)]:
            with self.subTest(output=output):
                self.assertEqual(ui_viewport(output), hud_canvas(output))
                self.assertEqual(ui_viewport(output, {'sharp_ui': '0'}), hud_canvas(output))

    def test_the_output_size_above_1080p(self):
        self.assertEqual(ui_viewport((2560, 1440), SHARP), (2560, 1440))
        self.assertEqual(ui_viewport((3840, 2160), SHARP), (3840, 2160))
        # 16:10: the 16:9 stage scaled to the width (Scaleform shows it whole).
        self.assertEqual(ui_viewport((2560, 1600), SHARP), (2560, 1440))

    def test_never_below_1080p_nor_on_ultrawide(self):
        self.assertEqual(ui_viewport((1920, 1080), SHARP), OUTPUT_SIZE)
        self.assertEqual(ui_viewport((1280, 720), SHARP), OUTPUT_SIZE)  # the Steam Deck keeps 1080 lines
        # An ultrawide output keeps its widened HUD canvas (its writes stay ultrawide_writes').
        self.assertEqual(ui_viewport((3440, 1440), SHARP), (2580, 1080))

    def test_the_resolution_patch_gets_the_viewport(self):
        writes = dict(resolution_writes(XML, (1706, 960), '01.09', SEGMENTS, ui_viewport((2560, 1440), SHARP)))
        self.assertEqual(writes[UI_WIDTH], bytes([0xB8]) + (2560).to_bytes(3, 'little'))
        self.assertEqual(writes[UI_HEIGHT], bytes([0xB9]) + (1440).to_bytes(3, 'little'))
        self.assertTrue(compile_patches(XML, [RESOLUTION_TEMPLATE], '01.09', SEGMENTS))
        self.assertGreater(UI_WIDTH + EBOOT_BASE, 0)


class SceneSizeTests(unittest.TestCase):
    def test_the_scene_is_never_a_ui_size(self):
        # Native AA at 1440p: the scene would be the UI viewport's size, indistinguishable from it.
        self.assertEqual(scaled_sizes({'output_res': '2560x1440', 'upscaler': 'off', **SHARP}),
                         ((2556, 1438), (2560, 1440)))
        # 4K Performance: the scene would be the stage's 1920x1080.
        self.assertEqual(scaled_sizes({'output_res': '3840x2160', 'preset': '3', **SHARP}),
                         ((1916, 1078), (3840, 2160)))

    def test_other_scenes_and_sharp_ui_off_stay(self):
        self.assertEqual(scaled_sizes({'output_res': '2560x1440', 'preset': '1', **SHARP}),
                         ((1706, 960), (2560, 1440)))
        self.assertEqual(scaled_sizes({'output_res': '2560x1440', 'upscaler': 'off'}),
                         ((2560, 1440), (2560, 1440)))


class RunTests(unittest.TestCase):
    def test_run_py_passes_the_viewport_and_keeps_the_startup_patch(self):
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        start = source.index('canvas = ui_viewport(output, read_settings(Path(config)))')
        block = source[start:start + 700]
        self.assertIn("live = '0'", block)
        self.assertIn("env['BB_UI_RES'] = f'{canvas[0]}x{canvas[1]}'", block)


if __name__ == '__main__':
    unittest.main()
