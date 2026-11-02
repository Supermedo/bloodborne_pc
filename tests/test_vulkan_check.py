"""scripts/vulkan_check.py: the start check asks for exactly what the game's renderer refuses to start
without, on the card the renderer would pick."""
from paths import ROOT
import json
import re
import subprocess
import sys
import unittest
from unittest import mock

import vulkan_check as V

RENDERER = ROOT / 'gpu' / 'shadps4' / 'video_core' / 'renderer_vulkan'
V12, V13, V14 = (1 << 22) | (2 << 12), V.API, (1 << 22) | (4 << 12)


def card(name='Card', api=V13, kind=V.DISCRETE, memory=8 << 30, extensions=V.REQUIRED_EXTENSIONS, features=None,
         vendor=None, device_id=None):
    return {'name': name, 'api': api, 'type': kind, 'memory': memory, 'extensions': set(extensions),
            'features': dict.fromkeys(V.REQUIRED_FEATURES, True) if features is None else features,
            'vendor': vendor, 'device_id': device_id}


def adapter(name, vendor, device_id, memory=512 << 20):
    return {'name': name, 'vendor': vendor, 'device_id': device_id, 'memory': memory}


class RendererListTests(unittest.TestCase):
    """When the renderer needs something new, this fails until the start check asks for it too."""

    def test_the_same_list_as_the_renderer(self):
        instance = (RENDERER / 'vk_instance.cpp').read_text(encoding='utf-8')
        platform = (RENDERER / 'vk_platform.cpp').read_text(encoding='utf-8')
        header = (RENDERER / 'vk_platform.h').read_text(encoding='utf-8')
        target = re.search(r'TargetVulkanApiVersion = VK_API_VERSION_(\d+)_(\d+);', header)
        self.assertEqual((1 << 22) | (int(target.group(2)) << 12), V.API)
        self.assertIn('available_version >= TargetVulkanApiVersion', platform)  # the loader
        self.assertIn('properties.apiVersion >= TargetVulkanApiVersion', instance)  # the card
        # VK_EXT_ROBUSTNESS_2_EXTENSION_NAME is "VK_EXT_robustness2": compared without case and underscores.
        macros = re.findall(r'ASSERT_MSG\(add_extension\((VK_\w+)_EXTENSION_NAME\)', instance)
        plain = lambda name: name.replace('_', '').lower()  # noqa: E731
        self.assertEqual([plain(m) for m in macros], [plain(n) for n in V.REQUIRED_EXTENSIONS])
        features = re.findall(r'ASSERT_MSG\(robustness2_features\.(\w+),', instance)
        self.assertEqual(tuple(features), V.REQUIRED_FEATURES)
        # Nothing else the renderer calls required that the check does not know about.
        self.assertEqual(len(re.findall(r'"Required Vulkan (?:extension|feature) unavailable', instance)),
                         len(V.REQUIRED_EXTENSIONS) + len(V.REQUIRED_FEATURES))

    def test_the_same_choice_of_card_as_the_renderer(self):
        instance = (RENDERER / 'vk_instance.cpp').read_text(encoding='utf-8')
        order = [instance.index(text) for text in ('left_supports_api', 'left_is_discrete', 'left_is_cpu',
                                                   'left_mem_size > right_mem_size')]
        self.assertEqual(order, sorted(order))  # the criteria ranked() uses, in its order


class VerdictTests(unittest.TestCase):
    def test_a_card_with_everything(self):
        result = V.evaluate(V14, [card(api=V14)])
        self.assertTrue(result['ok'])
        self.assertEqual(result['device']['missing'], [])

    def test_no_driver_or_an_old_loader(self):
        self.assertEqual(V.evaluate(None, [])['problem'], 'loader')
        self.assertEqual(V.evaluate(V12, [])['problem'], 'loader_version')
        self.assertEqual(V.evaluate(V13, [])['problem'], 'devices')

    def test_what_a_card_lacks_is_named(self):
        old = card('Old card', api=V12, extensions=['VK_KHR_swapchain'])
        result = V.evaluate(V13, [old])
        self.assertEqual(result['problem'], 'device')
        self.assertEqual(result['device']['missing'], ['Vulkan 1.3 (1.2.0)', 'VK_KHR_push_descriptor',
                                                       'VK_EXT_vertex_attribute_divisor', 'VK_EXT_robustness2'])
        partial = card(features={'robustBufferAccess2': True, 'robustImageAccess2': True, 'nullDescriptor': False})
        self.assertEqual(V.evaluate(V13, [partial])['device']['missing'], ['nullDescriptor'])

    def test_the_card_the_game_picks(self):
        laptop = [card('Intel UHD', kind=1, memory=1 << 30), card('GeForce', memory=6 << 30),
                  card('Old GeForce', api=V12, memory=12 << 30)]
        self.assertEqual(V.evaluate(V13, laptop)['device']['name'], 'GeForce')
        software = [card('llvmpipe', kind=V.CPU, memory=64 << 30), card('Radeon', kind=1, memory=2 << 30)]
        self.assertEqual(V.evaluate(V13, software)['device']['name'], 'Radeon')
        # BB_GPU_ID picks by number, as the game does; a number without a card stops it.
        self.assertEqual(V.evaluate(V13, laptop, '2')['device']['name'], 'Old GeForce')
        self.assertFalse(V.evaluate(V13, laptop, '2')['ok'])
        self.assertEqual(V.evaluate(V13, laptop, '7')['problem'], 'devices')


class UnseenCardTests(unittest.TestCase):
    """A card Windows has and Vulkan does not list: a player's RTX 5070 Ti was in Windows' list, but the game ran
    on the processor's AMD Radeon(TM) Graphics ("DLSS: unavailable (not an NVIDIA GPU)"), 30 October 2026."""
    RTX = adapter('NVIDIA GeForce RTX 5070 Ti', 0x10DE, 0x2C05, 16 << 30)
    APU = adapter('AMD Radeon(TM) Graphics', 0x1002, 0x13C0)

    def test_the_game_on_the_processors_graphics(self):
        apu = card('AMD Radeon(TM) Graphics', kind=1, memory=2 << 30, vendor=0x1002, device_id=0x13C0)
        result = V.evaluate(V13, [apu], adapters=[self.RTX, self.APU])
        self.assertTrue(result['ok'])  # it starts, on the slower one
        self.assertEqual(result['unseen'], ['NVIDIA GeForce RTX 5070 Ti'])

    def test_no_card_at_all(self):
        result = V.evaluate(V13, [], adapters=[self.RTX])
        self.assertEqual((result['problem'], result['unseen']), ('devices', ['NVIDIA GeForce RTX 5070 Ti']))

    def test_a_working_discrete_card_is_enough(self):
        rtx = card('NVIDIA GeForce RTX 5070 Ti', vendor=0x10DE, device_id=0x2C05)
        # The processor's graphics without a Vulkan driver does not matter while the GeForce runs the game.
        self.assertEqual(V.evaluate(V13, [rtx], adapters=[self.RTX, self.APU])['unseen'], [])

    def test_a_card_vulkan_lists_is_seen(self):
        old = card('NVIDIA GeForce GTX 760', api=V12, vendor=0x10DE, device_id=0x1187)
        result = V.evaluate(V13, [old], adapters=[adapter('NVIDIA GeForce GTX 760', 0x10DE, 0x1187)])
        self.assertEqual((result['problem'], result['unseen']), ('device', []))  # too old, not unseen
        # Without ids (the tests' cards), by name.
        self.assertEqual(V.evaluate(V13, [card('NVIDIA GeForce RTX 5070 Ti', kind=1)], adapters=[self.RTX])['unseen'],
                         [])

    def test_only_graphics_card_makers(self):
        apu = card('AMD Radeon(TM) Graphics', kind=1, vendor=0x1002, device_id=0x13C0)
        virtual = [adapter('Parsec Virtual Display Adapter', 0x1AB8, 0x0101),
                   adapter('Microsoft Remote Display Adapter', 0x1414, 0x008C),
                   adapter('DisplayLink USB Device', 0x17E9, 0x4300)]
        self.assertEqual(V.evaluate(V13, [apu], adapters=virtual)['unseen'], [])

    def test_a_card_its_driver_holds_back(self):
        # R29: a GTX 1060 with Vulkan 1.2 from an old driver; the game picks the Vega, which has 1.3.
        gtx = card('NVIDIA GeForce GTX 1060 6GB', api=V12, vendor=0x10DE, device_id=0x1C03, memory=6 << 30)
        vega = card('AMD Radeon(TM) Vega 8 Graphics', kind=1, vendor=0x1002, device_id=0x15DD, memory=2 << 30)
        result = V.evaluate(V13, [gtx, vega])
        self.assertEqual(result['device']['name'], 'AMD Radeon(TM) Vega 8 Graphics')
        self.assertEqual(result['held_back'], [{'name': 'NVIDIA GeForce GTX 1060 6GB',
                                                'missing': ['Vulkan 1.3 (1.2.0)']}])
        self.assertEqual(result['unseen'], [])  # Vulkan sees it
        # The game on a discrete card: nothing held back to say.
        self.assertNotIn('held_back', V.evaluate(V13, [card('GeForce RTX 3060'), vega]))
        self.assertEqual(V.evaluate(V13, [vega])['held_back'], [])

    def test_no_driver_at_all_is_said_otherwise(self):
        self.assertEqual(V.evaluate(None, [], adapters=[self.RTX])['unseen'], [])  # 'loader' names the fix


class RunLogTests(unittest.TestCase):
    """run.py names the loader's variables that hide graphics drivers in the game's log."""

    def test_the_variables_that_hide_drivers(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('bbport_run_vulkan', ROOT / 'run.py')
        run = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(run)
        self.assertIsNone(run.vulkan_driver_note({'PATH': 'x', 'VK_INSTANCE_LAYERS': 'y'}))
        self.assertEqual(run.vulkan_driver_note({'VK_ICD_FILENAMES': r'C:\amd\amd-vulkan64.json',
                                                 'VK_LOADER_DRIVERS_DISABLE': '*nvidia*'}),
                         r'Vulkan: the drivers it loads are limited by VK_ICD_FILENAMES=C:\amd\amd-vulkan64.json; '
                         'VK_LOADER_DRIVERS_DISABLE=*nvidia*')
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        self.assertLess(source.index('note = vulkan_driver_note(env)'),
                        source.index('probe = Path('))  # before anything can stop the start
        sys.path.insert(0, str(ROOT / 'launcher'))
        import bbport_help
        line = run.vulkan_driver_note({'VK_DRIVER_FILES': 'x.json'})
        self.assertEqual([topic for pattern, topic in bbport_help.LOG_HINTS if re.search(pattern, line)],
                         ['gpu_unseen'])


class LauncherTests(unittest.TestCase):
    """How the launcher reads the answer (bbport_launcher_win.vulkan_verdict)."""

    def answer(self, stdout, code):
        sys.path.insert(0, str(ROOT / 'launcher'))
        import bbport_launcher_win as L
        done = subprocess.CompletedProcess([], code, stdout, '')
        with mock.patch.object(L.subprocess, 'run', return_value=done):
            return L.vulkan_verdict({})

    def test_its_json_line(self):
        self.assertEqual(self.answer('loader noise\n{"ok": true, "problem": null}\n', 0), {'ok': True, 'problem': None})

    def test_a_driver_that_took_the_check_down(self):
        self.assertEqual(self.answer('', 0xC0000005), {'ok': False, 'problem': 'crashed', 'code': '0xC0000005'})

    def test_the_checks_own_failure_says_nothing(self):
        self.assertIsNone(self.answer('Traceback (most recent call last): ...', 1))


class ThisPcTests(unittest.TestCase):
    def test_it_answers_in_its_own_process(self):
        result = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'vulkan_check.py')], capture_output=True,
                                text=True, timeout=60)
        verdict = json.loads(result.stdout)
        self.assertEqual(result.returncode, 0 if verdict['ok'] else 2)
        self.assertIn(verdict['problem'], (None, 'loader', 'loader_version', 'devices', 'device'))
        self.assertIsInstance(verdict['unseen'], list)

    @unittest.skipUnless(sys.platform == 'win32', "Windows' own list of cards")
    def test_windows_lists_its_cards(self):
        adapters = V.windows_adapters()
        self.assertNotIn('Microsoft Basic Render Driver', [a['name'] for a in adapters])  # its software renderer
        _loader, devices = V.query()
        # Every card Vulkan lists here is one Windows has: the same ids.
        self.assertTrue({(d['vendor'], d['device_id']) for d in devices if d['type'] != V.CPU}
                        <= {(a['vendor'], a['device_id']) for a in adapters})


if __name__ == '__main__':
    unittest.main()
