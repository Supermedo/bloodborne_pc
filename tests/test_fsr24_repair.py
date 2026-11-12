# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'launcher'),str(ROOT/'scripts')]
import bbport_fsr41 as family
import bbport_lang

class RepairTests(unittest.TestCase):
    def test_family_snapshot_discovers_driver_fingerprint_once_for_both_methods(self):
        paths={'vulkan':'installed-vk.dll','dx12':'installed-dx.dll'}
        with patch.object(family.bbport_fsr411,'query',return_value={}), \
             patch.object(family.bbport_driver_runtime,'discover',return_value=paths) as discover, \
             patch.object(family.bbport_fsr412,'query_vulkan',return_value={'supported':True}) as vk, \
             patch.object(family.bbport_fsr412,'query_dx12',return_value={'supported':False}) as dx:
            result=family.query(Path('fixture'),env={},refresh=True)
        discover.assert_called_once_with()
        self.assertIs(vk.call_args.kwargs['paths'],paths)
        self.assertIs(dx.call_args.kwargs['paths'],paths)
        self.assertTrue(vk.call_args.kwargs['refresh'] and dx.call_args.kwargs['refresh'])
        self.assertTrue(result['fsr412_vulkan']['supported'])
        self.assertFalse(result['fsr412_dx12']['supported'])

    def test_restart_status_is_actionable_and_not_a_hardware_rejection(self):
        state={'supported':False,'reason':'restart'}
        self.assertEqual(family.code(state),family.RESTART)
        self.assertEqual(family.STATUS_KEYS[family.RESTART],'Restart required')
        self.assertEqual(family.COLORS[family.RESTART],'#d9a441')
        self.assertIsNone(family.preferred_route([state]*4))
        self.assertEqual(family.preferred_route([state,{'supported':True},state,state]),1)
        for language in family.TEXTS:
            self.assertEqual(bbport_lang.table(language)['Restart required'],family.RESTART_TEXTS[language])
            self.assertEqual(bbport_lang.table(language)[family.AUTO_EXPOSURE_KEY],family.AUTO_EXPOSURE_TEXTS[language])

if __name__=='__main__':unittest.main()
