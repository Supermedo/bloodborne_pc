# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression: a start/recheck must preserve four exact, independent provider results."""
import copy
from pathlib import Path
import queue
import sys
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'launcher'), str(ROOT / 'scripts')]
import bbport_fsr41 as family
import bbport_fsr411 as legacy
import bbport_fsr412 as driver
import bbport_launcher_win as launcher


def status(supported=True, reason='ready', detail=''):
    return dict(supported=supported, reason=reason, detail=detail)


def snapshot():
    return dict(vendor_id=0x1002, vk=status(), dx12=status(), dx12_interop=status(),
                fsr412_vulkan=status(), fsr412_dx12=status())


class DetectionSnapshotTests(unittest.TestCase):
    def test_present_driver_dlls_cannot_enable_an_unchecked_provider(self):
        # This is the old Play path: only the base 4.1.1 result remains.
        caps = dict(vendor_id=0x1002, vk=status(),
                    dx12=status(False, 'provider', '4.1.1 unavailable'))
        paths = dict(vulkan='present/amdvkffx64.dll', dx12='present/amdxcffx64.dll')
        for method in ('vk', 'dx12'):
            with self.subTest(method=method):
                result = driver.method_status(method, caps, paths)
                self.assertFalse(result['supported'])
                self.assertEqual(result['reason'], 'check')

    def test_query_refreshes_both_versions_without_mutating_the_base_probe_cache(self):
        base = dict(vendor_id=0x1002, vk=status(), dx12=status(), dx12_interop=status())
        original = copy.deepcopy(base)
        environment = {'BB_GPU_ID': '1'}
        with patch.object(legacy, 'query', return_value=base) as query, \
             patch.object(driver, 'query_vulkan', side_effect=[status(False, 'driver'), status()]) as vk, \
             patch.object(driver, 'query_dx12', side_effect=[status(), status(False, 'hardware')]) as dx:
            first = family.query(ROOT, environment, refresh=True)
            second = family.query(ROOT, environment, refresh=True)
        for probe in (query, vk, dx):
            self.assertEqual(probe.call_count, 2)
            self.assertTrue(probe.call_args.kwargs['refresh'])
        self.assertEqual(base, original)
        first['dx12']['supported'] = False
        self.assertTrue(base['dx12']['supported'])
        self.assertFalse(first['fsr412_vulkan']['supported'])
        self.assertTrue(second['fsr412_vulkan']['supported'])
        self.assertFalse(second['fsr412_dx12']['supported'])

    def test_412_dx12_can_work_when_only_the_411_provider_is_unavailable(self):
        caps = snapshot()
        caps['dx12'] = status(False, 'provider', '4.1.1 unavailable on this GPU/driver')
        self.assertTrue(driver.method_status('dx12', caps)['supported'])
        for missing in ('interop', 'adapter', 'files', 'check'):
            caps['dx12_interop'] = status(False, missing, 'specific sharing failure')
            result = driver.method_status('dx12', caps)
            self.assertFalse(result['supported'])
            self.assertEqual(result['reason'], missing)
        caps.pop('dx12_interop')
        self.assertEqual(driver.method_status('dx12', caps)['reason'], 'check')

    def test_each_exact_provider_keeps_its_own_failure(self):
        caps = snapshot()
        for method, key in (('vk', 'fsr412_vulkan'), ('dx12', 'fsr412_dx12')):
            for reason in ('driver', 'hardware', 'check'):
                with self.subTest(method=method, reason=reason):
                    current = copy.deepcopy(caps)
                    current[key] = status(False, reason, 'exact provider rejected')
                    self.assertEqual(driver.method_status(method, current), current[key])

    def make_launcher(self, version, backend):
        ui = launcher.Launcher.__new__(launcher.Launcher)
        ui.process = None
        ui.fsr411_downloading = False
        ui.app = dict(launcher.APP_DEFAULTS)
        ui.ini = dict(upscaler=version, fsr411_backend=backend, frame_gen='fsr402')
        values = {'upscaler': version, 'online_pool': 'supermedo'}
        ui.var = lambda key, *_: types.SimpleNamespace(get=lambda: values.get(key, ''))
        for name in ('collect', 'show', 'refresh_status', 'refresh_fsr411'):
            setattr(ui, name, Mock())
        return ui

    def test_play_refreshes_the_whole_snapshot_for_all_four_saved_methods(self):
        for version in ('fsr411', 'fsr412'):
            for backend in ('vk', 'dx12'):
                with self.subTest(version=version, backend=backend):
                    ui = self.make_launcher(version, backend)
                    caps = snapshot()
                    saved = dict(ui.ini)
                    # Stop at game-folder validation, after the real support boundary.
                    with patch.object(family, 'query', return_value=caps) as probe, \
                         patch.object(ui, 'fsr41_status', return_value=status()) as selected, \
                         patch.object(launcher, 'game_info', return_value=False), \
                         patch.object(launcher.subprocess, 'Popen') as popen:
                        ui.messagebox = types.SimpleNamespace(showerror=Mock())
                        ui.play()
                    self.assertIs(ui.fsr411_support, caps)
                    self.assertTrue(probe.call_args.kwargs['refresh'])
                    selected.assert_called_once_with(backend)
                    popen.assert_not_called()
                    self.assertEqual(ui.ini, saved)

    def test_play_rejects_unavailable_412_before_starting_and_preserves_preferences(self):
        for backend in ('vk', 'dx12'):
            ui = self.make_launcher('fsr412', backend)
            caps = snapshot()
            caps['fsr412_vulkan' if backend == 'vk' else 'fsr412_dx12'] = status(False, 'driver')
            saved = dict(ui.ini)
            with patch.object(family, 'query', return_value=caps), \
                 patch.object(launcher.subprocess, 'Popen') as popen:
                ui.play()
            popen.assert_not_called()
            ui.show.assert_called_once_with('graphics')
            self.assertEqual(ui.ini, saved)

    def test_recheck_replaces_all_four_results_together(self):
        ui = self.make_launcher('fsr411', 'vk')
        ui.fsr411_support = snapshot()
        ui.ui_calls = queue.Queue()
        caps = snapshot()
        caps['fsr412_dx12'] = status(False, 'driver')
        class ImmediateThread:
            def __init__(self, target, **kwargs): self.target = target
            def start(self): self.target()
        with patch.object(family, 'query', return_value=caps) as probe, \
             patch.object(launcher.threading, 'Thread', ImmediateThread):
            ui.recheck_fsr411()
        self.assertIsNone(ui.fsr411_support)
        ui.ui_calls.get_nowait()()
        self.assertIs(ui.fsr411_support, caps)
        self.assertTrue(probe.call_args.kwargs['refresh'])
        self.assertFalse(ui.fsr411_support['fsr412_dx12']['supported'])


if __name__ == '__main__':
    unittest.main()
