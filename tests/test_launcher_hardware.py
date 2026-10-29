"""The launcher's hardware check and the settings it recommends (launcher/bbport_hardware.py)."""
from paths import ROOT
import importlib.util
import unittest

spec = importlib.util.spec_from_file_location('bbport_hardware', ROOT / 'launcher/bbport_hardware.py')
hardware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardware)


def rate(card, cpu, ram):
    return hardware.assess(hardware.GPU_BY_ID[card], hardware.cpu_from_name(cpu), ram)


def profile(rating, display=(2560, 1440), current=None, goal='smooth'):
    available = hardware.outputs(*display)
    res = hardware.recommended_class(rating, available)
    return {key: value for _store, key, value, _why in hardware.profile(rating, res, available[res], current, goal)}


class TableTests(unittest.TestCase):
    """The tables of the hardware check on bbport.de/install."""

    def test_cards_and_scores(self):
        self.assertEqual(len(hardware.GPU_LIST), 161)
        self.assertEqual(hardware.GPU_BY_ID['amd-rx-9070-xt'].score, 275)
        self.assertEqual(hardware.GPU_BY_ID['nvidia-rtx-4060'].vram, 8)
        self.assertTrue(hardware.GPU_BY_ID['nvidia-rtx-4060'].dlss)
        self.assertTrue(hardware.GPU_BY_ID['amd-rx-7800-xt'].fsr4)
        self.assertFalse(hardware.GPU_BY_ID['amd-rx-6800-xt'].fsr4)

    def test_vulkan_names_match_the_table(self):
        for name, vram, expected in (('NVIDIA GeForce RTX 4070 Laptop GPU', 8, 'nvidia-rtx-4070-laptop'),
                                     ('AMD Radeon RX 9070 XT', 16, 'amd-rx-9070-xt'),
                                     ('AMD Radeon RX 580 Series', 8, 'amd-rx-580-8-gb'),
                                     ('Intel(R) Arc(TM) A770 Graphics', 16, 'intel-arc-a770-16-gb'),
                                     ('AMD Radeon(TM) 890M Graphics', 0, 'amd-radeon-890m-ryzen-ai-300')):
            self.assertEqual(hardware.match_gpu(name, vram).id, expected, name)
        self.assertIsNone(hardware.match_gpu('AMD Radeon(TM) Graphics', 0))

    def test_processors(self):
        for name, score, support in (('12th Gen Intel(R) Core(TM) i7-12700K', 195, 'ok'),
                                     ('Intel(R) Core(TM) i5-3570K CPU @ 3.40GHz', 60, 'compat'),
                                     ('Intel(R) Core(TM) i7 CPU 920 @ 2.67GHz', 0, 'none'),
                                     ('AMD Ryzen 7 5800X 8-Core Processor', 180, 'ok'),
                                     ('AMD FX(tm)-8350 Eight-Core Processor', 0, 'none'),
                                     ('Intel(R) Celeron(R) N4020 CPU @ 1.10GHz', 0, 'none')):
            cpu = hardware.cpu_from_name(name)
            self.assertEqual((cpu.score, cpu.support), (score, support), name)

    def test_laptop_processors_count_lower(self):
        """The tables score desktop chips; a laptop's H-class gets four fifths, its U-class two thirds."""
        for name, score, laptop, label in (
                ('Intel(R) Core(TM) i7-7820HK CPU @ 2.90GHz', 84, 'h', 'Intel Core i7-7820HK'),
                ('Intel(R) Core(TM) i7-7700K CPU @ 4.20GHz', 105, None, 'Intel Core i7-7700K'),
                ('Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz', 70, 'u', 'Intel Core i5-8250U'),
                ('Intel(R) Core(TM) i7-1065G7 CPU @ 1.30GHz', 86, 'u', 'Intel Core i7-1065G7'),
                ('Intel(R) Core(TM) i7-4710MQ CPU @ 2.50GHz', 70, 'h', 'Intel Core i7-4710MQ'),
                ('13th Gen Intel(R) Core(TM) i9-13980HX', 230, None, 'Intel Core i9-13980HX'),
                ('AMD Ryzen 7 5800H with Radeon Graphics', 144, 'h', 'AMD Ryzen 7 5800H'),
                ('AMD Ryzen 7 7840HS w/ Radeon 780M Graphics', 178, 'h', 'AMD Ryzen 7 7840HS'),
                ('AMD Ryzen 5 5600G with Radeon Graphics', 160, None, 'AMD Ryzen 5 5600G'),
                ('AMD Ryzen 7 5800X3D 8-Core Processor', 180, None, 'AMD Ryzen 7 5800X3D')):
            cpu = hardware.cpu_from_name(name)
            self.assertEqual((cpu.score, cpu.laptop, cpu.label), (score, laptop, label), name)

    def test_the_hard_limits_follow_the_processors_instructions(self):
        def cpu(avx=True, bmi1=True, movbe=True, lzcnt=True):
            def cpuid(leaf, _subleaf=0):
                return {0: (0x16, 0, 0, 0), 0x80000000: (0x80000008, 0, 0, 0),
                        1: (0, 0, (avx << 28) | (avx << 27) | (movbe << 22), 0), 7: (0, bmi1 << 3, 0, 0),
                        0x80000001: (0, 0, lzcnt << 5, 0)}.get(leaf, (0, 0, 0, 0))
            return hardware.instruction_lines(hardware.instructions(cpuid))
        self.assertEqual(cpu()[0][0], 'ok')
        self.assertEqual(cpu(bmi1=False, movbe=False, lzcnt=False)[0], (
            'warn', 'Your processor lacks BMI1, MOVBE, LZCNT: the port translates them (compatibility mode), so '
                    'expect bugs.'))
        self.assertEqual(cpu(avx=False)[0][0], 'bad')
        self.assertEqual(hardware.instruction_lines(None), [])

    def test_the_laptops_dedicated_card_wins(self):
        lines = ['AMD Radeon(TM) Graphics, integrated, 512 MiB', 'NVIDIA GeForce RTX 4060 Laptop GPU, discrete, 8188 MiB']
        self.assertEqual(hardware.best_gpu_line(lines), lines[1])

    def test_vendor(self):
        self.assertEqual(hardware.vendor_of('AMD Radeon RX 9070 XT'), 'amd')
        self.assertEqual(hardware.vendor_of('NVIDIA GeForce RTX 3060'), 'nvidia')
        self.assertEqual(hardware.vendor_of('Intel(R) Arc(TM) A770 Graphics'), 'intel')


class RatingTests(unittest.TestCase):
    def test_tiers(self):
        top = rate('amd-rx-9070-xt', 'AMD Ryzen 7 5800X 8-Core Processor', 32)
        self.assertEqual((top.tier_label, top.levels), ('4K-ready', {'1080p': 5, '1440p': 5, '4K': 4}))
        low = rate('nvidia-gtx-1650', 'Intel(R) Core(TM) i5-9400F CPU @ 2.90GHz', 16)
        self.assertEqual((low.tier_label, low.limit), ('Minimum', 'gpu'))
        self.assertEqual(rate('amd-rx-9070-xt', 'AMD FX(tm)-8350 Eight-Core Processor', 32).tier, 0)
        self.assertEqual(rate('amd-rx-9070-xt', 'AMD Ryzen 7 5800X 8-Core Processor', 8).limit, 'ram')


def rate_detected(vulkan_name, vram, cpu, ram):
    """The rating the launcher shows for what it detects: the card's Vulkan name and memory."""
    return hardware.assess(hardware.match_gpu(vulkan_name, vram), hardware.cpu_from_name(cpu), ram)


class PlayersPcTests(unittest.TestCase):
    """PCs that run the game, rated "This PC can't run the port." since 1.9.3: under 12 GB of RAM and
    under 4 GB of video memory counted as unsupported. Both are below the minimum; only no Vulkan 1.3,
    a card far too slow or a processor without AVX stop the port."""

    def assertRunsBelowTheMinimum(self, r, limit):
        self.assertEqual((r.tier, r.tier_label, r.limit), (1, 'Below minimum', limit))
        self.assertNotEqual(r.summary, hardware.SUMMARIES[0])
        self.assertNotIn('bad', [state for state, _part, _text in r.checks])
        self.assertTrue(r.rows[0][3], 'no settings at 1080p')  # the row shows what to set

    def test_an_rtx_3050_laptop_with_8_gb(self):
        """A player's laptop that plays completely smooth: RTX 3050 Laptop 4 GB, Core i5 13th gen, 8 GB."""
        r = rate_detected('NVIDIA GeForce RTX 3050 Laptop GPU', 4, '13th Gen Intel(R) Core(TM) i5-13420H', 8)
        self.assertEqual(r.gpu.id, 'nvidia-rtx-3050-laptop')
        self.assertRunsBelowTheMinimum(r, 'ram')
        self.assertIn(('warn', 'ram', '8 GB is below the minimum of 16 GB: the game may close right after PLAY '
                                      '(code 3).'), r.checks)
        self.assertEqual(r.levels['1080p'], 2)  # the card and processor decide the frame rate, not the RAM
        settings = profile(r, (1920, 1080))
        self.assertEqual((settings['output_res'], settings['upscaler'], settings['preset'], settings['fps_mode']),
                         ('1920x1080', 'fsr3', '3', '60'))

    def test_a_gtx_1060_3_gb(self):
        """A player's GTX 1060 3 GB that got up to the Blood-Starved Beast on older versions."""
        r = rate_detected('NVIDIA GeForce GTX 1060 3GB', 3, 'Intel(R) Core(TM) i5-8400 CPU @ 2.80GHz', 16)
        self.assertEqual(r.gpu.id, 'nvidia-gtx-1060-3-gb')
        self.assertRunsBelowTheMinimum(r, 'gpu')
        self.assertIn(('warn', 'gpu', 'Below the minimum (GTX 1050 Ti / GTX 1650 / RX 570).'), r.checks)
        self.assertTrue(any(note.startswith('Settings for 4 GB cards') for note in r.notes))
        settings = profile(r, (1920, 1080))  # the 4 GB card settings at the low end: 720p
        self.assertEqual((settings['output_res'], settings['upscaler'], settings['preset'], settings['fps_mode']),
                         ('1280x720', 'fsr3', '3', '60'))
        self.assertEqual(settings['model_lod'], '0')
        self.assertFalse(settings['object_motion'])

    def test_an_rx_570_with_a_ryzen_3_3200g_and_8_gb(self):
        r = rate_detected('AMD Radeon RX 570 Series', 8, 'AMD Ryzen 3 3200G with Radeon Vega Graphics', 8)
        self.assertEqual(r.gpu.id, 'amd-rx-570-8-gb')
        self.assertRunsBelowTheMinimum(r, 'ram')
        self.assertEqual(r.levels['1080p'], 2)
        settings = profile(r, (1920, 1080))
        self.assertEqual((settings['output_res'], settings['upscaler'], settings['preset'], settings['fps_mode']),
                         ('1920x1080', 'fsr3', '3', '30'))
        self.assertEqual(settings['model_lod'], '1')

    def test_no_vulkan_1_3_still_stops_the_port(self):
        r = rate_detected('NVIDIA GeForce GTX 780', 3, 'AMD Ryzen 7 5800X 8-Core Processor', 32)
        self.assertEqual(r.gpu.id, 'nvidia-gtx-780')
        self.assertEqual((r.tier, r.tier_label, r.summary), (0, 'Not supported', hardware.SUMMARIES[0]))
        self.assertIn(('bad', 'gpu', 'Not supported: no Vulkan 1.3 driver for this card.'), r.checks)
        self.assertEqual(set(r.levels.values()), {0})

    def test_only_what_stops_the_port_rates_a_pc_unsupported(self):
        cpu = hardware.cpu_from_name('AMD Ryzen 7 5800X 8-Core Processor')
        for card in hardware.GPU_LIST:
            for ram in (4, 8, 12, 16):
                r = hardware.assess(card, cpu, ram)
                blocked = not card.vulkan13 or card.score < 25
                self.assertEqual(r.tier == 0, blocked, (card.label, ram))
                self.assertEqual(r.levels['1080p'] > 0, not blocked, (card.label, ram))


class ProfileTests(unittest.TestCase):
    def test_a_strong_radeon_at_1440p(self):
        settings = profile(rate('amd-rx-9070-xt', 'AMD Ryzen 7 5800X 8-Core Processor', 32))
        self.assertEqual(settings['output_res'], '2560x1440')
        self.assertEqual((settings['upscaler'], settings['preset'], settings['fps_mode']), ('fsr4', '1', 'uncap'))
        self.assertEqual(settings['model_lod'], '-2')
        self.assertTrue(settings['object_motion'] and settings['effect_ssao'])
        self.assertFalse(settings['effect_game_aa'])

    def test_four_gigabyte_cards_follow_the_website(self):
        settings = profile(rate('nvidia-gtx-1650', 'Intel(R) Core(TM) i5-9400F CPU @ 2.90GHz', 16), (1920, 1080))
        self.assertEqual((settings['upscaler'], settings['preset'], settings['fps_mode']), ('fsr3', '3', '60'))
        self.assertEqual(settings['model_lod'], '0')
        self.assertFalse(settings['object_motion'])

    def test_intel_graphics_without_an_upscaler(self):
        settings = profile(rate('intel-arc-a770-16-gb', 'Intel(R) Core(TM) i5-13400F', 16))
        self.assertEqual(settings['upscaler'], 'off')
        self.assertNotIn('preset', settings)
        self.assertTrue(settings['effect_game_aa'])

    def test_dlss_only_on_a_geforce(self):
        for card in hardware.GPU_LIST:
            if not card.vulkan13:
                continue
            r = hardware.assess(card, hardware.cpu_from_name('AMD Ryzen 7 5800X'), 32)
            if r.tier == 0:
                continue
            upscaler = profile(r, (3840, 2160))['upscaler']
            self.assertTrue(upscaler != 'dlss' or card.vendor == 'nvidia', card.label)
            self.assertTrue(upscaler != 'fsr4' or card.fsr4, card.label)

    def test_taste_is_left_alone(self):
        settings = profile(rate('amd-rx-9070-xt', 'AMD Ryzen 7 5800X 8-Core Processor', 32))
        for key in ('effect_chromatic_aberration', 'effect_dof', 'effect_motion_blur', 'sharpen', 'sharpness'):
            self.assertNotIn(key, settings)
        self.assertNotIn('effect_ssr', settings)  # off only where the card has no room for it
        weaker = rate('nvidia-rtx-4060', 'AMD Ryzen 5 5600X', 16)
        self.assertFalse(profile(weaker, current={'effect_ssr': True})['effect_ssr'])

    def test_frame_generation_off_only_where_it_cannot_work(self):
        weak = rate('nvidia-gtx-1650', 'Intel(R) Core(TM) i5-9400F CPU @ 2.90GHz', 16)
        self.assertEqual(profile(weak, (1920, 1080), {'frame_gen': 'fsr3'})['frame_gen'], 'off')
        strong = rate('nvidia-rtx-4070', 'AMD Ryzen 7 5800X', 32)
        self.assertNotIn('frame_gen', profile(strong, current={'frame_gen': 'dlss'}))

    def test_a_processor_below_the_minimum_sets_the_frame_rate_not_the_picture(self):
        """A player's GTX 1080 laptop with an i7-7820HK: 21-28 FPS in every test run, from the recommended
        settings down to 640x360. Expect under 30 FPS and cap at 30; the picture follows the card."""
        r = rate('nvidia-gtx-1080', 'Intel(R) Core(TM) i7-7820HK CPU @ 2.90GHz', 32)
        self.assertEqual((r.tier_label, r.limit, r.levels['1080p'], r.picture_levels['1080p']),
                         ('Below minimum', 'cpu', 1, 3))
        self.assertTrue(any(part == 'cpu' and 'Laptop processor' in text for _s, part, text in r.checks))
        settings = profile(r)
        self.assertEqual((settings['output_res'], settings['fps_mode'], settings['preset']), ('1920x1080', '30', '2'))
        self.assertTrue(settings['effect_ssao'] and settings['effect_dynamic_shadows'])
        self.assertEqual(settings['model_lod'], '0')
        same_card = profile(rate('nvidia-gtx-1080', 'AMD Ryzen 7 5800X', 32))
        for key in ('output_res', 'preset', 'effect_ssao', 'effect_dynamic_shadows', 'model_lod', 'object_motion'):
            self.assertEqual(settings[key], same_card[key], key)

    def test_the_frame_rate_cap_follows_the_processor(self):
        for cpu, fps in (('Intel(R) Core(TM) i7-7820HK CPU @ 2.90GHz', '30'),  # below the minimum
                         ('Intel(R) Core(TM) i3-10100 CPU @ 3.60GHz', '60'),  # the minimum: 30-60 FPS
                         ('Intel(R) Core(TM) i5-11400F CPU @ 2.60GHz', 'uncap'),  # above it: 60-120 FPS
                         ('AMD Ryzen 7 5800X 8-Core Processor', 'uncap')):
            self.assertEqual(profile(rate('nvidia-rtx-4070', cpu, 32))['fps_mode'], fps, cpu)

    def test_a_card_below_the_minimum_still_goes_to_720p(self):
        r = rate('nvidia-gtx-1630', 'AMD Ryzen 7 5800X', 16)
        self.assertEqual((r.tier_label, r.limit, r.picture_levels['1080p']), ('Below minimum', 'gpu', 1))
        self.assertEqual(profile(r, (1920, 1080))['output_res'], '1280x720')

    def test_ultrawide_outputs(self):
        self.assertEqual(hardware.outputs(3440, 1440), {'1080p': '2560x1080', '1440p': '3440x1440'})
        self.assertEqual(hardware.outputs(1920, 1080), {'1080p': '1920x1080'})


class GoalTests(unittest.TestCase):
    """The player's goal (Play page): smooth, balanced (60 FPS, best picture), quiet (60 FPS, less work)."""

    def test_a_strong_pc(self):
        r = rate('amd-rx-9070-xt', 'AMD Ryzen 7 5800X 8-Core Processor', 32)
        smooth, balanced, quiet = (profile(r, goal=g) for g in hardware.GOALS)
        self.assertEqual((smooth['preset'], smooth['fps_mode'], smooth['model_lod']), ('1', 'uncap', '-2'))
        self.assertEqual((balanced['preset'], balanced['fps_mode'], balanced['model_lod']), ('0', '60', '-2'))
        self.assertEqual((quiet['preset'], quiet['fps_mode'], quiet['model_lod']), ('2', '60', '0'))
        self.assertTrue(smooth['object_motion'] and balanced['object_motion'])
        self.assertFalse(quiet['object_motion'])

    def test_balanced_gives_more_detail_only_with_room(self):
        r = rate('nvidia-rtx-4060', 'Intel Core i5-12400F', 16)
        smooth, balanced = profile(r), profile(r, goal='balanced')
        self.assertEqual(balanced['preset'], smooth['preset'])  # no headroom for Native AA
        self.assertEqual(balanced['fps_mode'], '60')

    def test_a_weak_pc_keeps_its_cap(self):
        r = rate('nvidia-gtx-1650', 'Intel Core i3-10100', 16)
        for goal in hardware.GOALS:
            self.assertIn(profile(r, display=(1920, 1080), goal=goal)['fps_mode'], ('30', '60'))

    def test_unknown_goal_is_smooth(self):
        r = rate('amd-rx-9070-xt', 'AMD Ryzen 7 5800X 8-Core Processor', 32)
        self.assertEqual(profile(r, goal='fast'), profile(r))
        self.assertEqual(hardware.GOAL_FPS, {'smooth': 120, 'balanced': 60, 'quiet': 60})


if __name__ == '__main__':
    unittest.main()
