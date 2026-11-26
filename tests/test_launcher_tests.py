"""The launcher's game tests: what they run and how they read the game (launcher/bbport_benchmark.py),
and the report (launcher/bbport_report.py). The game itself is not started here."""
from paths import ROOT
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'launcher/{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bench = load('bbport_benchmark')
report = load('bbport_report')

LOG = '''Overlay: menu ready
Frame stats: 142.2 FPS, worst frame 64.1 ms (vblank 480 Hz); 0 shader/pipeline compiles, 0.0 ms; 1349 recorder syncs; 0 write faults/s, 0 hot pages; GPU thread 0.00 us/draw, 11 draws/frame, idle 57.5%; blocked: recorder 1.1%
Frame pacing: median 6.61 ms, stddev 3.25 ms, min 0.00 ms, p1 2.02 ms, p99 16.78 ms, 16 frames over 1.5x median
Frame stats: 98.0 FPS, worst frame 21.5 ms (vblank 480 Hz); 37 shader/pipeline compiles, 215.3 ms; 1364 recorder syncs; 0 write faults/s, 0 hot pages; GPU thread 0.00 us/draw, 861 draws/frame, idle 1.3%; blocked: recorder 1.2%
Frame pacing: median 10.20 ms, stddev 1.03 ms, min 4.03 ms, p1 5.04 ms, p99 13.11 ms, 3 frames over 1.5x median
'''


def result(fps, p99=None, **settings):
    out = {'fps': fps, 'settings': settings, 'status': 'ok'}
    if p99:
        out['p99_ms'] = p99
    return out


def drive(plan, measure):
    """Runs a plan against `measure(settings) -> result`; the names and settings it tried."""
    results, tried = [], []
    for name, settings in plan.steps(results):
        tried.append((name, settings))
        results.append({**measure(settings), 'settings': settings})
    return tried


class ReadingTests(unittest.TestCase):
    def test_frame_statistics(self):
        frames, p99 = bench.stats(LOG)
        self.assertEqual(frames, [(142.2, 11.0), (98.0, 861.0)])
        self.assertEqual(p99, [16.78, 13.11])
        m = list(bench.FRAME_STATS.finditer(LOG))[1]
        self.assertEqual((m[2], m[3], m[5]), ('21.5', '37', '861'))
        self.assertEqual(bench.FRAME_PACING.findall(LOG)[1], ('10.20', '1.03', '13.11', '3'))

    def test_ini(self):
        text = bench.ini_text('upscaler=fsr4\npreset=1\n# note\nobject_motion=1\n',
                              {'preset': '0', 'object_motion': False, 'cheat_no_death': True})
        self.assertEqual(text, 'upscaler=fsr4\npreset=0\n# note\nobject_motion=0\ncheat_no_death=1\n')

    def test_routes(self):
        pan = bench.pan_route(24)
        self.assertEqual(bench.route_seconds(pan), 24.5)
        self.assertTrue(all(len(line.split()) == 8 for line in pan.splitlines()))

    def test_builtin_scenarios(self):
        with tempfile.TemporaryDirectory() as folder:
            found = {s.id: s for s in bench.scenarios(folder)}
        self.assertEqual(set(found), {'hunters-view', 'cathedral-ward'})  # only finished benchmarks ship
        view = found['hunters-view']
        self.assertEqual(view.save, 'own')
        self.assertEqual(view.steps[0]['do'], 'title')
        self.assertTrue(view.loop)
        self.assertEqual(view.route('pan:24'), bench.pan_route(24))
        self.assertTrue(bench.FIXED_INI['cheat_stealth'] and bench.FIXED_INI['cheat_no_death'])
        # saves only inside the folder of a scenario that starts from its own (Cathedral Ward)
        own = [scenario.save_folder() for scenario in found.values() if scenario.save not in ('own', 'ask')]
        self.assertFalse([p for p in (ROOT / 'launcher/assets/tests').rglob('userdata*')
                          if not any(folder in p.parents for folder in own)])
        ward = found['cathedral-ward']
        self.assertTrue(ward.port_details and (ward.save_folder() / 'userdata0010').is_file())
        self.assertEqual([s['name'] for s in ward.steps if s['do'] == 'measure'], ['standing', 'camera turn', 'walk'])
        known = {'title', 'wait', 'press', 'hold', 'route', 'measure', 'capture'}
        for scenario in found.values():
            self.assertEqual(scenario.missing_routes(), [])
            for step in scenario.steps + scenario.loop:
                self.assertIn(step['do'], known)
                if step['do'] in ('route', 'measure'):
                    self.assertGreater(bench.route_seconds(scenario.route(step['route'])), 20)

    def test_mod_scenario_and_recorded_routes(self):
        """A scenario whose save comes from a mod (never shipped), and a route the player recorded."""
        with tempfile.TemporaryDirectory() as folder:
            tests = Path(folder) / 'tests'
            (tests / 'scenarios').mkdir(parents=True)
            (tests / 'scenarios' / 'run.json').write_text(json.dumps({
                'id': 'run', 'name': 'Run', 'save': 'ask',
                'save_from': {'name': 'Boss saves', 'url': 'https://example.org/mod', 'file': 'Main Story BOSSES',
                              'folder': 'Cleric Beast'},
                'steps': [{'do': 'title'}, {'do': 'measure', 'route': 'run.txt', 'captures': [30, 60]}],
                'loop': [{'do': 'measure', 'route': 'run.txt'}]}))
            run = {s.id: s for s in bench.scenarios(tests / 'scenarios')}['run']
            self.assertEqual(run.save_from['folder'], 'Cleric Beast')
            self.assertEqual(run.missing_routes(), ['run.txt'])
            own = tests / 'routes' / 'run' / 'run.txt'
            own.parent.mkdir(parents=True)
            own.write_text('0 0 128 0 128 128 0 0\n120000 0 128 128 128 128 0 0\n')
            self.assertEqual(run.missing_routes(), [])
            self.assertEqual(bench.route_seconds(run.route('run.txt')), 120)

    def test_mod_save(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'bosses'
            for name in ('Dungeon BOSSES/Dungeon BOSS 9 - Blood-starved Beast', 'Main Story BOSSES/BOSS 1 - Cleric Beast',
                         'Main Story BOSSES/BOSS 2 - Father Gascoigne', 'Main Story BOSSES/BOSS 3 - Blood Starved Beast'):
                (root / name / 'sce_sys').mkdir(parents=True)
                for file in ('userdata0000', 'userdata0010', 'backup0000', 'boss.png', 'sce_sys/param.sfo'):
                    (root / name / file).write_text(name)
            cleric = bench.mod_save(root, 'Cleric Beast', 'Main Story BOSSES')
            self.assertEqual(cleric.name, 'BOSS 1 - Cleric Beast')
            self.assertEqual(bench.mod_save(root, 'Gascoigne').name, 'BOSS 2 - Father Gascoigne')
            # the same boss in both archives: the scenario's archive first
            self.assertEqual(bench.mod_save(root, 'Blood Starved Beast', 'Main Story BOSSES').name,
                             'BOSS 3 - Blood Starved Beast')
            self.assertEqual(bench.mod_save(root, 'Blood Starved Beast', 'Dungeon BOSSES').name,
                             'Dungeon BOSS 9 - Blood-starved Beast')
            self.assertEqual(bench.mod_save(cleric, 'Cleric Beast', 'Main Story BOSSES'), cleric)  # its own folder
            self.assertIsNone(bench.mod_save(cleric, 'Gascoigne'))
            self.assertIsNone(bench.mod_save(root, 'Moon Presence'))
            self.assertIsNone(bench.mod_save(root / 'missing', 'Cleric Beast'))
            # the test copies only the game's files of a save from elsewhere into the player's slot
            user = Path(folder) / 'user'
            slot = user / 'savedata' / '1' / 'CUSA03173' / 'SPRJ0005'
            slot.mkdir(parents=True)
            (slot / 'userdata0000').write_text('own')
            (Path(folder) / 'ask.json').write_text(json.dumps({'id': 'ask', 'save': 'ask', 'steps': [{'do': 'title'}]}))
            scenario = bench.Scenario(Path(folder) / 'ask.json')
            test = bench.Test(folder, [], {}, user, '', '', scenario, None, Path(folder) / 'out', print, save=str(cleric))
            test.work = Path(folder) / 'work'
            test.prepare(slot)
            copied = test.work / 'user' / 'savedata' / '1' / 'CUSA03173' / 'SPRJ0005'
            self.assertEqual(sorted(p.name for p in copied.iterdir()), ['backup0000', 'userdata0000', 'userdata0010'])
            self.assertEqual((copied / 'userdata0000').read_text(), 'Main Story BOSSES/BOSS 1 - Cleric Beast')
            self.assertEqual((slot / 'userdata0000').read_text(), 'own')

    def test_player_input_stops_a_run(self):
        pad = bench.XINPUT_GAMEPAD
        self.assertFalse(bench.pad_in_use(pad(0, 0, 0, 3000, -5000, 8000, -8000)))  # a resting stick jitters
        self.assertTrue(bench.pad_in_use(pad(0x1000, 0, 0, 0, 0, 0, 0)))            # a button
        self.assertTrue(bench.pad_in_use(pad(0, 0, 0, 0, 0, 20000, 0)))             # the camera stick
        self.assertTrue(bench.pad_in_use(pad(0, 0, 200, 0, 0, 0, 0)))               # a trigger

        class Guard:
            def touched(self):
                return 'keyboard or mouse'
        run = bench.Run(type('T', (), {'cancelled': False})(), 1, 'run', {})
        run.guard = Guard()
        with self.assertRaises(bench.Stop) as stop:
            run.check()
        self.assertEqual(stop.exception.status, 'input')

    def test_player_scenarios(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'walk').mkdir()
            (Path(folder) / 'walk' / 'scenario.json').write_text(json.dumps(
                {'name': 'Walk', 'save': 'save', 'steps': [{'do': 'title'}]}))
            (Path(folder) / 'broken.json').write_text('{')
            found = {s.id: s for s in bench.scenarios(folder)}
            self.assertIn('scenario', found)
            self.assertEqual(found['scenario'].name, 'Walk')
            self.assertEqual(found['scenario'].save_folder(), Path(folder) / 'walk' / 'save')
            self.assertNotIn('broken', found)


class PlanTests(unittest.TestCase):
    START = {'upscaler': 'fsr4', 'preset': '1', 'object_motion': True, 'effect_ssao': True,
             'effect_dynamic_shadows': True, 'model_lod': '0'}

    def test_goal_kept_goes_up(self):
        plan = bench.Plan(self.START, 60)
        tried = drive(plan, lambda s: {'fps': 120 if s['preset'] != '0' else 80, 'p99_ms': 12})
        self.assertEqual(tried[0][0], 'recommended')
        self.assertEqual(tried[1][1]['preset'], '0')  # Native AA next
        self.assertEqual(plan.choice['settings']['model_lod'], '0')
        self.assertFalse([s for _name, s in tried if s.get('model_lod') == '-2'])

    def test_goal_missed_steps_down(self):
        speeds = {('1', True): 50, ('1', False): 56, ('2', False): 66}

        def measure(s):
            if s.get('effect_ssao') is False and s['preset'] == '3':
                return {'fps': 90, 'p99_ms': 14}  # the lowest settings
            fps = speeds.get((s['preset'], s['object_motion']), 50)
            return {'fps': fps + (6 if s.get('effect_ssao') is False else 0), 'p99_ms': 20}
        plan = bench.Plan(self.START, 60)
        tried = drive(plan, measure)
        self.assertEqual([n for n, _s in tried][:3], ['recommended', 'lowest', 'object_motion False'])
        self.assertIsNotNone(plan.choice)
        self.assertGreaterEqual(plan.choice['fps'], 60 * 1.03)

    def test_processor_bound_lowers_the_goal(self):
        plan = bench.Plan(self.START, 120)
        drive(plan, lambda s: {'fps': 100 if s['preset'] == '3' else 99, 'p99_ms': 12})
        self.assertAlmostEqual(plan.goal, 97.0)
        self.assertEqual(plan.floor, 100)

    def test_failed_first_run_stops(self):
        plan = bench.Plan(self.START, 60)
        tried = drive(plan, lambda s: {'error': 'the game did not start'})
        self.assertEqual(len(tried), 1)
        self.assertIsNone(plan.choice)

    def test_variants_and_soak(self):
        plan = bench.Variants({'preset': '1'}, [('Native AA', {'preset': '0'}), ('Quality', {'preset': '1'})], 2)
        tried = drive(plan, lambda s: {'fps': 100})
        self.assertEqual([n for n, _s in tried], ['Native AA', 'Quality', 'Native AA', 'Quality'])
        self.assertEqual(tried[0][1], {'preset': '0'})
        self.assertEqual(drive(bench.Soak({}), lambda s: {'fps': 100}), [('stability', {})])


class ReportTests(unittest.TestCase):
    def run_result(self, index, name, fps, settings, status='ok'):
        frames = [{'t': 5.0 * i, 'fps': fps + i, 'worst_ms': 20.0, 'compiles': i, 'compile_ms': 0.0, 'draws': 800.0}
                  for i in range(1, 8)]
        pacing = [{'t': 5.0 * i, 'median_ms': 10.0, 'stddev_ms': 1.0, 'p99_ms': 14.0, 'stutters': 2}
                  for i in range(1, 8)]
        telemetry = [{'t': float(t), 'ram_gb': 5.0 + t / 100, 'vram_gb': 6.5, 'cpu': 40.0, 'gpu': 90.0,
                      'free_gb': 20.0, 'hung': False} for t in range(1, 36)]
        return {'index': index, 'name': name, 'settings': settings, 'status': status, 'windows': [['m', 10, 30]],
                'frames': frames, 'pacing': pacing, 'telemetry': telemetry, 'captures': {}, 'log': f'run-{index}.log',
                'events': [{'t': 12.0, 'text': 'Texture cache: memory pressure, 7000 of 8000 MiB'}], 'seconds': 35,
                'fps': fps, 'p99_ms': 14.0, 'low1': 71.4, 'worst_ms': 20.0, 'stutters': 14, 'compiles': 28,
                'upscaler': 'FSR 4', 'hints': ['vram'], 'message': '' if status == 'ok' else 'the game ended (code 3)'}

    def test_report_and_csv(self):
        sys.modules.setdefault('bbport_help', load('bbport_help'))
        with tempfile.TemporaryDirectory() as folder:
            runs = [self.run_result(1, 'recommended', 96.0, {'preset': '1'}),
                    self.run_result(2, 'preset -1', 92.0, {'preset': '0'}, status='crash')]
            path = report.write(folder, {'kind': 'performance', 'scenario': "Hunter's view", 'goal': 60,
                                         'start': {'preset': '1'}, 'choice': 1, 'date': '2026-10-20 12:00'}, runs)
            page = Path(path).read_text(encoding='utf-8')
            for text in ('Performance test', 'Best for 60 FPS', 'Frame rate over time', 'Graphics memory (VRAM)',
                         'Crashed', 'Preset Native AA', 'memory runs full', '<svg'):
                self.assertIn(text, page)
            rows = (Path(folder) / 'samples.csv').read_text(encoding='utf-8').splitlines()
            self.assertTrue(rows[0].startswith('run,name,seconds,fps'))
            self.assertGreater(len(rows), 60)
            zipped = report.export(folder, Path(folder) / 'out.zip')
            self.assertTrue(zipped.is_file())

    def test_axis_rounding(self):
        self.assertEqual([report.nice(v) for v in (0.7, 7, 55, 101, 160)], [0.8, 8, 60, 120, 160])


if __name__ == '__main__':
    unittest.main()
