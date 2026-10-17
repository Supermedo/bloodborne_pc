"""Neural-rendering settings of the launcher: ini keys, option lists and the game's key names."""
import importlib.util
import inspect
import os
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('launcher_win', ROOT / 'launcher/bbport_launcher_win.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

SETTINGS_SOURCE = (ROOT / 'gpu/shim/bbport_neural_settings.cpp').read_text(encoding='utf-8')


class LauncherNeuralSettingsTests(unittest.TestCase):
    def test_ini_defaults_include_neural_keys(self):
        self.assertEqual(launcher.INI_DEFAULTS['nr_enabled'], '0')
        self.assertEqual(launcher.INI_DEFAULTS['nr_preset'], '0')
        self.assertEqual(launcher.INI_DEFAULTS['nr_input_scale'], '1.00')
        self.assertEqual(launcher.INI_DEFAULTS['nr_resolve'], 'auto')
        self.assertEqual(launcher.INI_DEFAULTS['nr_ui_correction'], '1')
        self.assertEqual(launcher.INI_DEFAULTS['nr_hotkey'], '86')  # SDL_SCANCODE_KP_MINUS
        self.assertEqual(launcher.INI_DEFAULTS['nr_passes'], '1')
        self.assertEqual(launcher.INI_DEFAULTS['nr_transfer_strength'], '1.00')
        self.assertEqual(launcher.INI_DEFAULTS['nr_colour_strength'], '1.00')
        self.assertEqual(launcher.INI_DEFAULTS['nr_max_ratio'], '2.00')
        self.assertEqual(launcher.INI_DEFAULTS['nr_white_point'], '1.00')

    def test_pass_defaults(self):
        for i in range(1, launcher.NR_PASS_COUNT + 1):
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_enabled'], '1')
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_stage'], 'post')
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_style'], '0')
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_intensity'], '1.00')
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_structure'], '1.00')
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_tone'], '1.00')
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_auto_skin'], '0')
            self.assertEqual(launcher.INI_DEFAULTS[f'nr_pass{i}_skin_structure'], '1.00')

    def test_flags_and_floats_cover_the_keys(self):
        for key in ('nr_enabled', 'nr_ui_correction', 'nr_pass1_enabled', 'nr_pass4_auto_skin'):
            self.assertIn(key, launcher.INI_FLAGS)
            self.assertNotIn(key, launcher.INI_FLOATS)
        for key in ('nr_input_scale', 'nr_transfer_strength', 'nr_colour_strength', 'nr_max_ratio',
                    'nr_white_point', 'nr_pass1_intensity', 'nr_pass4_skin_structure'):
            self.assertIn(key, launcher.INI_FLOATS)
            self.assertNotIn(key, launcher.INI_FLAGS)
        # sharpness keeps its old treatment, now through the shared float set.
        self.assertIn('sharpness', launcher.INI_FLOATS)

    def test_option_lists_match_the_settings_module(self):
        self.assertEqual([k for k, _ in launcher.NR_PRESETS], ['0', '1', '2'])
        self.assertEqual([k for k, _ in launcher.NR_RESOLVE], ['auto', 'residual', 'ratio'])
        self.assertEqual([k for k, _ in launcher.NR_STAGES], ['pre', 'post', 'present'])
        self.assertEqual([k for k, _ in launcher.NR_STYLES], ['0', '1', '2'])
        self.assertEqual(launcher.NR_PASS_COUNT, 4)

    def test_nr_model_version_threshold(self):
        for version, expected in ((None, False), ((310, 8, 0, 0), False),
                                  ((310, 9, 15, 99), False), ((310, 9, 16, 0), True),
                                  ((311, 0, 0, 0), True)):
            self.assertEqual(launcher.nr_optimized_supported(version), expected)
        self.assertEqual([label[0] for _, label in launcher.NR_PRESETS],
                         ['Automatic', 'Original', 'Optimized'])

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher keyboard mapping')
    def test_hotkey_codes_use_sdl_keyboard_mapping(self):
        try:
            launcher.nr_keyboard()
        except OSError:
            self.skipTest('SDL3.dll is not available in this source-test environment')
        for key, code in (('F6', 63), ('KP_Subtract', 86), ('Control_R', 228),
                          ('semicolon', 51), ('F24', 115)):
            with self.subTest(key=key):
                self.assertEqual(launcher.nr_scancode(key), code)

    @unittest.skipUnless(os.name == 'nt', 'Windows physical keyboard mapping')
    def test_hotkey_uses_physical_key_on_non_us_layout(self):
        try:
            launcher.nr_keyboard()
        except OSError:
            self.skipTest('SDL3.dll is not available in this source-test environment')
        # German Z occupies the US Y position. Store the physical SDL Y scancode,
        # which is what the game's key event reports, rather than the Z glyph.
        with mock.patch.object(launcher.ctypes.windll.user32, 'MapVirtualKeyW', return_value=0x15):
            self.assertEqual(launcher.nr_scancode('z', 0x5a), 28)

    def test_pass_chain_is_tabs_not_four_cards(self):
        source = inspect.getsource(launcher.Launcher.build_graphics)
        self.assertIn("self.nr_tabs = ttk.Notebook(holder, style='Neural.TNotebook')", source)
        self.assertIn('self.nr_add_button', source)
        self.assertIn('self.nr_delete_button', source)
        self.assertIn('self.nr_rebuild_tabs()', source)
        # The cluttered per-pass card rows and the pass-count combo box are gone.
        self.assertNotIn("self.choice(f, 'nr_passes'", source)
        self.assertNotIn("_('Pass {} enabled')", source)
        for name in ('nr_add_pass', 'nr_delete_pass', 'nr_rebuild_tabs', 'nr_pass_values'):
            self.assertTrue(callable(getattr(launcher.Launcher, name)))

    def test_delete_shifts_later_passes_and_defaults_the_last_slot(self):
        values = dict(launcher.INI_DEFAULTS)
        values.update({'nr_passes': '3', 'nr_pass1_style': '0', 'nr_pass2_style': '1',
                       'nr_pass3_style': '2', 'nr_pass1_intensity': '0.10',
                       'nr_pass2_intensity': '0.20', 'nr_pass3_intensity': '0.30',
                       'nr_pass2_enabled': '0', 'nr_pass3_auto_skin': '1',
                       'nr_pass3_stage': 'present', 'nr_enabled': '1', 'nr_resolve': 'ratio'})
        out = launcher.nr_pass_delete(values, 2, 3)
        self.assertEqual(out['nr_passes'], '2')
        # Pass 3 moved up into slot 2 and kept every one of its values.
        self.assertEqual(out['nr_pass2_style'], '2')
        self.assertEqual(out['nr_pass2_intensity'], '0.30')
        self.assertEqual(out['nr_pass2_auto_skin'], '1')
        self.assertEqual(out['nr_pass2_stage'], 'present')
        # The freed last slot is back to its defaults.
        for field, default in launcher.NR_PASS_DEFAULTS:
            self.assertEqual(out[f'nr_pass3_{field}'], default, field)
        # Pass 1 and the settings outside the chain are untouched.
        self.assertEqual(out['nr_pass1_style'], '0')
        self.assertEqual(out['nr_pass1_intensity'], '0.10')
        self.assertEqual(out['nr_enabled'], '1')
        self.assertEqual(out['nr_resolve'], 'ratio')
        self.assertEqual(sorted(out), sorted(values))

    def test_delete_first_pass_moves_the_whole_chain_up(self):
        values = dict(launcher.INI_DEFAULTS)
        values.update({'nr_passes': '4', 'nr_pass1_tone': '0.11', 'nr_pass2_tone': '0.22',
                       'nr_pass3_tone': '0.33', 'nr_pass4_tone': '0.44'})
        out = launcher.nr_pass_delete(values, 1, 4)
        self.assertEqual(out['nr_passes'], '3')
        self.assertEqual(out['nr_pass1_tone'], '0.22')
        self.assertEqual(out['nr_pass2_tone'], '0.33')
        self.assertEqual(out['nr_pass3_tone'], '0.44')
        for field, default in launcher.NR_PASS_DEFAULTS:
            self.assertEqual(out[f'nr_pass4_{field}'], default, field)

    def test_delete_refuses_the_only_pass_and_add_stops_at_four(self):
        values = dict(launcher.INI_DEFAULTS)
        self.assertEqual(launcher.nr_pass_delete(values, 1, 1)['nr_passes'], '1')
        self.assertEqual(launcher.nr_pass_add(values, launcher.NR_PASS_COUNT)['nr_passes'], '1')

    def test_add_defaults_the_new_slot_and_keeps_the_others(self):
        values = dict(launcher.INI_DEFAULTS)
        values.update({'nr_passes': '2', 'nr_pass2_style': '0', 'nr_pass2_tone': '0.40',
                       'nr_pass3_style': '1', 'nr_pass4_intensity': '0.90'})
        out = launcher.nr_pass_add(values, 2)
        self.assertEqual(out['nr_passes'], '3')
        self.assertEqual(out['nr_pass2_style'], '0')
        self.assertEqual(out['nr_pass2_tone'], '0.40')
        for field, default in launcher.NR_PASS_DEFAULTS:
            self.assertEqual(out[f'nr_pass3_{field}'], default, field)
        # Slots past the new count keep whatever they held: they are ignored by the game.
        self.assertEqual(out['nr_pass4_intensity'], '0.90')
        self.assertEqual(sorted(out), sorted(values))

    def test_value_text_normalizes_flags_and_floats(self):
        self.assertEqual(launcher.nr_value_text('nr_pass1_enabled', True), '1')
        self.assertEqual(launcher.nr_value_text('nr_pass1_enabled', False), '0')
        self.assertEqual(launcher.nr_value_text('nr_pass1_enabled', '1'), '1')
        self.assertEqual(launcher.nr_value_text('nr_pass1_enabled', '0'), '0')
        self.assertEqual(launcher.nr_value_text('nr_pass1_intensity', 0.5), '0.50')
        self.assertEqual(launcher.nr_value_text('nr_pass1_intensity', '0.25'), '0.25')
        self.assertEqual(launcher.nr_value_text('nr_pass1_stage', 'pre'), 'pre')

    def test_slider_ranges_are_the_verified_limits(self):
        # The style parameters are verified at 0..1, skin structure runs -1..1, and the Ratio shader
        # clamps transfer and colour to 0..1: a wider slider would do nothing.
        self.assertEqual(launcher.NR_STYLE_MAX, 1.0)
        self.assertEqual((launcher.NR_SKIN_MIN, launcher.NR_SKIN_MAX), (-1.0, 1.0))
        self.assertEqual(launcher.NR_TRANSFER_MAX, 1.0)
        self.assertEqual(launcher.NR_COLOUR_MAX, 1.0)
        self.assertEqual(launcher.NR_MAX_RATIO, (1.0, 8.0))
        self.assertEqual(launcher.NR_WHITE_POINT, (0.01, 100.0))
        source = inspect.getsource(launcher.Launcher.nr_rebuild_tabs)
        self.assertNotIn('2.0', source)
        self.assertNotIn('advanced options', source)

    def test_keys_are_the_ones_the_game_reads(self):
        for key in launcher.NR_DEFAULTS:
            pass_key = re.fullmatch(r'nr_pass(\d+)_(\w+)', key)
            literal = f'nr_pass%d_{pass_key.group(2)}' if pass_key else key
            self.assertIn(literal, SETTINGS_SOURCE, key)
        # And the other way round: every pass field the game writes is exposed by the launcher.
        fields = set(re.findall(r'nr_pass%d_(\w+)', SETTINGS_SOURCE))
        self.assertEqual(fields, {_field for _field, _default in launcher.NR_PASS_DEFAULTS})
        plain = {key for key in re.findall(r'"(nr_[a-z0-9_]+)"', SETTINGS_SOURCE) if key != 'nr_pass'}
        self.assertTrue(plain <= set(launcher.NR_DEFAULTS), plain - set(launcher.NR_DEFAULTS))

    def test_ini_roundtrip_keeps_neural_values(self):
        previous = os.environ.get('BB_CONFIG')
        try:
            with tempfile.TemporaryDirectory() as folder:
                os.environ['BB_CONFIG'] = str(Path(folder) / 'bbport.ini')
                values = dict(launcher.INI_DEFAULTS)
                values.update({'nr_enabled': '1', 'nr_preset': '2', 'nr_resolve': 'ratio', 'nr_passes': '3',
                               'nr_input_scale': '0.75', 'nr_pass2_stage': 'present'})
                launcher.save_ini(values, [])
                loaded, lines = launcher.load_ini()
                self.assertEqual(loaded['nr_enabled'], '1')
                self.assertEqual(loaded['nr_preset'], '2')
                self.assertEqual(loaded['nr_resolve'], 'ratio')
                self.assertEqual(loaded['nr_passes'], '3')
                self.assertEqual(loaded['nr_input_scale'], '0.75')
                self.assertEqual(loaded['nr_pass2_stage'], 'present')
                self.assertTrue(any(line.startswith('nr_enabled=1') for line in lines))
        finally:
            if previous is None:
                os.environ.pop('BB_CONFIG', None)
            else:
                os.environ['BB_CONFIG'] = previous


class PassChainWidgetTests(unittest.TestCase):
    """Build the real pass tab widgets in a withdrawn Tk root (no window is shown)."""

    def setUp(self):
        try:
            import tkinter
            from tkinter import ttk
        except ImportError:  # pragma: no cover - headless Python without Tk
            self.skipTest('tkinter is not available')
        try:
            self.root = tkinter.Tk()
        except tkinter.TclError:  # pragma: no cover - no display
            self.skipTest('no display for Tk')
        self.root.withdraw()
        window = launcher.Launcher.__new__(launcher.Launcher)
        window.root = self.root
        window.tk, window.ttk = tkinter, ttk
        window.dpi = 1.0
        window.vars = {}
        window.ini = dict(launcher.INI_DEFAULTS)
        window.app = dict(launcher.APP_DEFAULTS)
        holder = ttk.Frame(self.root)
        window.nr_tabs = ttk.Notebook(holder)
        window.nr_tabs.pack(side='top', fill='x')
        window.nr_add_button = ttk.Button(holder, text='+ Add pass', command=window.nr_add_pass)
        window.nr_delete_button = ttk.Button(holder, text='Delete current pass',
                                             command=window.nr_delete_pass)
        window.nr_count_label = ttk.Label(holder, text='')
        self.window = window

    def tearDown(self):
        if getattr(self, 'root', None) is not None:
            self.root.update_idletasks()
            self.root.destroy()

    def test_tabs_follow_add_delete_and_keep_shifted_values(self):
        w = self.window
        w.nr_rebuild_tabs()
        self.assertEqual(len(w.nr_tabs.tabs()), 1)
        self.assertEqual(w.nr_tabs.tab(0, 'text'), 'Pass 1')
        w.nr_add_pass()
        self.assertEqual(len(w.nr_tabs.tabs()), 2)
        self.assertEqual(w.var('nr_passes', 'ini').get(), '2')
        self.assertEqual(w.nr_tabs.tab(1, 'text'), 'Pass 2')
        self.assertEqual(w.nr_tabs.index(w.nr_tabs.select()), 1)  # the new tab is selected
        # Edit pass 2, then delete pass 1: the edit moves up with the pass.
        w.var('nr_pass2_intensity', 'ini').set(0.4)
        w.var('nr_pass2_style', 'ini').set('0')
        w.nr_tabs.select(0)
        w.nr_delete_pass()
        self.assertEqual(len(w.nr_tabs.tabs()), 1)
        self.assertEqual(w.var('nr_passes', 'ini').get(), '1')
        self.assertAlmostEqual(float(w.var('nr_pass1_intensity', 'ini').get()), 0.4, places=2)
        self.assertEqual(w.var('nr_pass1_style', 'ini').get(), '0')
        self.assertEqual(w.ini['nr_pass2_style'], launcher.INI_DEFAULTS['nr_pass2_style'])
        self.assertIn('disabled', w.nr_delete_button.state())

    def test_add_button_stops_at_four_passes(self):
        w = self.window
        w.nr_rebuild_tabs()
        for _ in range(6):
            w.nr_add_pass()
        self.assertEqual(len(w.nr_tabs.tabs()), launcher.NR_PASS_COUNT)
        self.assertEqual(w.var('nr_passes', 'ini').get(), str(launcher.NR_PASS_COUNT))
        self.assertIn('disabled', w.nr_add_button.state())
        # Deleting the current (last) tab frees the slot and re-enables adding.
        w.nr_tabs.select(launcher.NR_PASS_COUNT - 1)
        w.nr_delete_pass()
        self.assertEqual(len(w.nr_tabs.tabs()), launcher.NR_PASS_COUNT - 1)
        # ttk.state() lists the active flags: enabled is the absence of 'disabled'.
        self.assertNotIn('disabled', w.nr_add_button.state())
        self.assertEqual(w.var('nr_passes', 'ini').get(), str(launcher.NR_PASS_COUNT - 1))

    def test_graphics_page_builds_the_neural_subsection(self):
        """The whole Graphics page is built in the withdrawn root: the new subsection must not
        raise, and the pass tabs follow the buttons."""
        w = self.window
        w.content = w.ttk.Frame(self.root)
        w.pages = {}
        w.fsr4_label = w.fsr4_button = w.fsr4_progress = None
        w.build_graphics()
        self.assertEqual(len(w.nr_tabs.tabs()), 1)
        self.assertEqual(w.nr_tabs.tab(0, 'text'), 'Pass 1')
        w.nr_add_pass()
        w.nr_add_pass()
        self.assertEqual(len(w.nr_tabs.tabs()), 3)
        self.assertEqual(w.var('nr_passes', 'ini').get(), '3')
        # Every neural key the game reads is present in the page's ini mirror.
        for key in launcher.NR_DEFAULTS:
            self.assertIn(key, w.ini)
        # The neural sliders cover exactly the verified ranges: -1 (skin) through 1 (style), and the
        # Ratio white point is the only one that reaches 100.
        scales = self.scales_of(w.pages['graphics'])
        self.assertAlmostEqual(min(float(scale['from']) for scale in scales), launcher.NR_SKIN_MIN)
        self.assertAlmostEqual(max(float(scale['to']) for scale in scales), launcher.NR_WHITE_POINT[1])
        self.assertTrue(any(float(scale['to']) == launcher.NR_MAX_RATIO[1] for scale in scales))

    def test_pass_tab_colors_match_the_action_buttons(self):
        w = self.window
        w.style()
        style = w.ttk.Style(self.root)
        for option in ('background', 'foreground'):
            self.assertEqual(style.lookup('Neural.TNotebook.Tab', option, ('selected',)),
                             style.lookup('NeuralAdd.TButton', option))
            self.assertEqual(style.lookup('Neural.TNotebook.Tab', option),
                             style.lookup('NeuralDelete.TButton', option))

    def test_custom_hotkey_capture_clear_and_escape(self):
        w = self.window
        w.content = w.ttk.Frame(self.root)
        w.content.pack(fill='both', expand=True)
        w.pages = {}
        w.build_graphics()
        self.root.deiconify()
        self.root.update()
        self.root.focus_force()
        self.root.update()
        self.assertEqual(w.var('nr_hotkey', 'ini').get(), '86')
        # Capture a key using the same Tk event path as a real Set… press.
        with mock.patch.object(launcher, 'nr_scancode', return_value=63) as convert:
            w.start_nr_hotkey_capture()
            self.root.event_generate('<KeyPress-F6>')
            self.root.update()
            convert.assert_called_once()
        self.assertEqual(w.var('nr_hotkey', 'ini').get(), '63')
        self.assertNotIn('disabled', w.nr_hotkey_button.state())
        w.start_nr_hotkey_capture()
        self.root.event_generate('<KeyPress-Escape>')
        self.root.update()
        self.assertEqual(w.var('nr_hotkey', 'ini').get(), '63')
        w.start_nr_hotkey_capture()
        w.clear_nr_hotkey()
        self.assertEqual(w.var('nr_hotkey', 'ini').get(), '0')
        self.assertEqual(w.nr_hotkey_label.cget('text'), '(none)')
        self.assertIsNone(self.root.grab_current())

    def test_optimized_preset_availability_preserves_saved_choice(self):
        w = self.window
        w.content = w.ttk.Frame(self.root)
        w.pages = {}
        w.ini['nr_preset'] = '2'
        for version, disabled in ((None, True), ((310, 8, 0, 0), True),
                                  ((310, 9, 16, 0), False)):
            with mock.patch.object(launcher, 'nr_runtime_version', return_value=version):
                w.build_graphics()
                self.assertEqual('disabled' in w.nr_preset_buttons['2'].state(), disabled)
                self.assertNotIn('disabled', w.nr_preset_buttons['0'].state())
                self.assertNotIn('disabled', w.nr_preset_buttons['1'].state())
                self.assertEqual(w.var('nr_preset', 'ini').get(), '2')

    def test_custom_hotkey_is_kept_when_graphics_opens(self):
        w = self.window
        w.ini['nr_hotkey'] = '63'
        w.content = w.ttk.Frame(self.root)
        w.pages = {}
        w.build_graphics()
        self.assertEqual(w.var('nr_hotkey', 'ini').get(), '63')

    def scales_of(self, widget):
        """Every ttk.Scale below `widget`, as {from, to} strings."""
        found = []
        for child in widget.winfo_children():
            if isinstance(child, self.window.ttk.Scale):
                found.append({'from': child.cget('from'), 'to': child.cget('to')})
            found.extend(self.scales_of(child))
        return found

    def test_pass_tab_sliders_use_the_verified_ranges(self):
        w = self.window
        w.nr_rebuild_tabs()
        tab = w.nr_tabs.nametowidget(w.nr_tabs.tabs()[0])
        scales = self.scales_of(tab)
        self.assertEqual(len(scales), 4)  # intensity, structure, tone, skin structure
        for scale in scales:
            self.assertLessEqual(float(scale['to']), launcher.NR_STYLE_MAX)
            self.assertGreaterEqual(float(scale['from']), launcher.NR_SKIN_MIN)
        self.assertEqual(sum(1 for scale in scales if float(scale['from']) < 0.0), 1)


if __name__ == '__main__':
    unittest.main()
