"""R82: the performance estimate and the automatic settings for common PCs (launcher/bbport_hardware.py)."""
from paths import ROOT
import importlib.util
import unittest

spec = importlib.util.spec_from_file_location('bbport_hardware', ROOT / 'launcher/bbport_hardware.py')
hardware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardware)


def detected(vulkan_name, integrated, vram):
    return hardware.match_gpu(vulkan_name, vram) or hardware.generic_gpu(vulkan_name, integrated, vram)


def rate(vulkan_name, vram, cpu, ram=16, integrated=False):
    return hardware.assess(detected(vulkan_name, integrated, vram), hardware.cpu_from_name(cpu), ram)


def settings(rating, display=(1920, 1080)):
    available = hardware.outputs(*display)
    res = hardware.recommended_class(rating, available)
    return {key: value for _store, key, value, _why in hardware.profile(rating, res, available[res])}


class PlayersCardsTests(unittest.TestCase):
    """Players ran these at 55–65 FPS while the launcher said "About 30 FPS" and capped them at 30."""

    def test_rx_580_gtx_1060_6_gb_and_gtx_1660_get_60(self):
        for name, vram in (('AMD Radeon RX 580 2048SP', 8), ('NVIDIA GeForce GTX 1060 6GB', 6),
                           ('NVIDIA GeForce GTX 1660', 6)):
            r = rate(name, vram, 'AMD Ryzen 5 3600 6-Core Processor')
            self.assertEqual((r.tier_label, r.levels['1080p']), ('Above minimum', 3), name)
            self.assertEqual(settings(r)['fps_mode'], '60', name)

    def test_arc_b580_with_an_i5_10400f_is_unlocked(self):
        r = rate('Intel(R) Arc(TM) B580 Graphics', 12, 'Intel(R) Core(TM) i5-10400F CPU @ 2.90GHz')
        self.assertEqual((r.tier_label, r.levels['1080p']), ('Above minimum', 4))  # was Minimum, 30–60
        self.assertEqual(settings(r)['fps_mode'], 'uncap')

    def test_these_processors_are_above_the_minimum(self):
        for name in ('Intel(R) Core(TM) i5-10400F CPU @ 2.90GHz', 'AMD Ryzen 5 3600 6-Core Processor',
                     'Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz', 'Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz'):
            self.assertEqual(hardware.cpu_tier(hardware.cpu_from_name(name)), 3, name)

    def test_integrated_graphics_stay_at_most_above_minimum(self):
        for card in hardware.GPU_LIST:
            if card.igpu:
                self.assertLessEqual(hardware.gpu_tier(card), 3, card.label)

    def test_a_4_gb_rtx_gets_dlss(self):
        r = rate('NVIDIA GeForce RTX 3050 Laptop GPU', 4, 'AMD Ryzen 5 3600 6-Core Processor')
        self.assertEqual(settings(r)['upscaler'], 'dlss')
        r = rate('NVIDIA GeForce GTX 1650', 4, 'AMD Ryzen 5 3600 6-Core Processor')
        self.assertEqual(settings(r)['upscaler'], 'fsr3')

    def test_lower_model_detail_only_for_integrated_and_weak_cards(self):
        cpu = 'AMD Ryzen 5 3600 6-Core Processor'
        self.assertEqual(settings(rate('AMD Radeon RX 570 Series', 8, cpu))['model_lod'], '0')
        self.assertEqual(settings(rate('AMD Radeon(TM) Graphics', 0, cpu, integrated=True))['model_lod'], '1')


class OutputTests(unittest.TestCase):
    def test_never_an_output_above_the_display(self):
        for width, height in ((1366, 768), (1600, 900), (1280, 1024), (1920, 1080), (2560, 1440)):
            for output in hardware.outputs(width, height).values():
                w, h = map(int, output.split('x'))
                self.assertLessEqual((w, h), (width, height), output)
        self.assertEqual(hardware.outputs(1366, 768)['1080p'], '1366x768')

    def test_the_output_follows_the_card_not_the_processor(self):
        r = rate('AMD Radeon RX 7900 XTX', 24, 'Intel(R) Core(TM) i5-4460 CPU @ 3.20GHz')
        self.assertLess(r.levels['4K'], r.picture_levels['4K'])
        available = hardware.outputs(3840, 2160)
        self.assertEqual(hardware.recommended_class(r, available), '4K')


class GenericCardTests(unittest.TestCase):
    """Cards not in the table used to get no rating and kept the defaults, every effect on."""

    def test_unknown_cards_get_a_rating(self):
        cpu = 'AMD Ryzen 5 3600 6-Core Processor'
        for name, integrated, vram in (('AMD Custom GPU 0405', True, 1), ('AMD Radeon(TM) Graphics', True, 0),
                                       ('AMD Radeon RX 7600S', False, 8), ('AMD Radeon RX 9060', False, 8),
                                       ('NVIDIA GeForce RTX 2050', False, 4)):
            self.assertIsNone(hardware.match_gpu(name, vram), name)
            r = rate(name, vram, cpu, integrated=integrated)
            self.assertGreater(r.tier, 0, name)
            self.assertEqual(r.gpu.group, hardware.GENERIC)
            self.assertNotIn('(R)', r.gpu.label)

    def test_the_maker_features_still_apply(self):
        self.assertTrue(detected('NVIDIA GeForce RTX 2050', False, 4).dlss)
        self.assertTrue(detected('AMD Radeon RX 9060', False, 8).fsr4)
        self.assertFalse(detected('AMD Custom GPU 0405', True, 1).fsr4)

    def test_more_video_memory_rates_higher(self):
        tiers = [hardware.gpu_tier(hardware.generic_gpu('AMD Radeon RX 9999', False, vram)) for vram in (4, 6, 8, 16)]
        self.assertEqual(tiers, sorted(tiers))

    def test_software_devices_get_none(self):
        for name in ('Microsoft Basic Render Driver', 'llvmpipe (LLVM 15.0.7, 256 bits)'):
            self.assertIsNone(hardware.generic_gpu(name, True, 0))


if __name__ == '__main__':
    unittest.main()
