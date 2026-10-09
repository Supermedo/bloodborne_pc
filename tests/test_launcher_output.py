"""Exercise output selection callbacks without opening a Tk window."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('output_launcher', ROOT / 'launcher/bbport_launcher_win.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class Variable:
    def __init__(self, value):
        self.value = value
        self.callbacks = []

    def get(self):
        return self.value

    def set(self, value):
        self.value = value
        for callback in self.callbacks:
            callback()

    def trace_add(self, _mode, callback):
        self.callbacks.append(callback)


class Combobox:
    def __init__(self, _parent, **options):
        self.options = options
        self.text = ''
        self.events = {}

    def current(self, index=None):
        if index is not None:
            self.text = self.options['values'][index]
        return self.options['values'].index(self.text) if self.text in self.options['values'] else -1

    def set(self, text):
        self.text = text

    def get(self):
        return self.text

    def bind(self, event, callback):
        self.events[event] = callback


class OutputSelectionTests(unittest.TestCase):
    def selection(self, value, key='output_res', options=None):
        window = launcher.Launcher.__new__(launcher.Launcher)
        variable = Variable(value)
        window.var = lambda *_: variable
        window.ttk = SimpleNamespace(Combobox=Combobox)
        return variable, window.choice(None, key, 'ini', options or launcher.OUTPUTS)

    def test_saved_custom_size_is_preserved(self):
        variable, box = self.selection('3840x1600')
        self.assertEqual(variable.get(), '3840x1600')
        self.assertEqual(box.get(), '3840x1600')
        self.assertEqual(box.options['state'], 'normal')

    def test_typing_commits_on_return_and_focus_loss(self):
        for event in ('<Return>', '<FocusOut>'):
            variable, box = self.selection('1920x1080')
            box.set(' 3840 X 1600 ')
            box.events[event](None)
            self.assertEqual(variable.get(), '3840x1600')

    def test_invalid_size_restores_saved_value(self):
        for invalid in ('1921x1080', '0x0', '7682x4320', '3840x1600junk', 'bad'):
            variable, box = self.selection('3840x1600')
            box.set(invalid)
            box.events['<Return>'](None)
            self.assertEqual(variable.get(), '3840x1600')
            self.assertEqual(box.get(), '3840x1600')

    def test_preset_selection_updates_saved_size(self):
        variable, box = self.selection('3840x1600')
        box.current(0)
        box.events['<<ComboboxSelected>>'](None)
        self.assertEqual(variable.get(), launcher.OUTPUTS[0][0])

    def test_other_selectors_remain_readonly(self):
        variable, box = self.selection('dlss', 'upscaler', launcher.UPSCALERS)
        self.assertEqual(box.options['state'], 'readonly')
        self.assertNotIn('<Return>', box.events)


if __name__ == '__main__':
    unittest.main()
