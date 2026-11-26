# SPDX-License-Identifier: GPL-3.0-or-later
"""SDK files, real query boundary, cache invalidation, and actionable failures."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'launcher'),str(ROOT/'scripts')]
import bbport_fsr412 as sdk

class Sdk24Tests(unittest.TestCase):
    def test_missing_sdk_is_not_reported_as_unsupported_hardware(self):
        with tempfile.TemporaryDirectory() as temp:
            result=sdk.query_vulkan(Path(temp), paths={}, env={}, refresh=True)
        self.assertEqual(result['reason'],'files')
        self.assertIn('amd_fidelityfx_upscaler_vk.dll',result['detail'])

    def test_sdk_query_works_without_internal_driver_filename_discovery(self):
        with tempfile.TemporaryDirectory() as temp:
            port=Path(temp); runtime=port/'bin/fsr24/amd_fidelityfx_upscaler_vk.dll'
            runtime.parent.mkdir(parents=True);runtime.write_bytes(b'mocked-runtime')
            exe=port/'bin/bb-gpu-capabilities.exe';exe.write_bytes(b'mocked-helper')
            response={'supported':True,'reason':'ready','detail':'4.1.2','checks':[]}
            completed=subprocess.CompletedProcess([],0,'SDK provider information\n'+json.dumps(response),'')
            with patch.object(sdk.subprocess,'run',return_value=completed) as run:
                result=sdk.query_vulkan(port,paths={},env={},refresh=True)
            self.assertTrue(result['supported'])
            self.assertNotIn('BB_TEST_AMD_VK_RUNTIME',run.call_args.kwargs['env'])
            self.assertIn('SDK 2.4',result['checks'][0]['detail'])

    def test_runtime_change_invalidates_support_snapshot(self):
        with tempfile.TemporaryDirectory() as temp:
            port=Path(temp); runtime=port/'bin/fsr24/amd_fidelityfx_upscaler_dx12.dll'
            runtime.parent.mkdir(parents=True);runtime.write_bytes(b'first')
            (port/'bin/bb-gpu-capabilities.exe').write_bytes(b'helper')
            good=subprocess.CompletedProcess([],0,json.dumps(dict(supported=True,reason='ready',detail='4.1.2')),'')
            bad=subprocess.CompletedProcess([],0,json.dumps(dict(supported=False,reason='provider',detail='No 4.1.2')),'')
            with patch.object(sdk.subprocess,'run',side_effect=[good,bad]) as run:
                self.assertTrue(sdk.query_dx12(port,paths={},env={})['supported'])
                self.assertTrue(sdk.query_dx12(port,paths={},env={})['supported'])
                runtime.write_bytes(b'replaced-runtime')
                self.assertFalse(sdk.query_dx12(port,paths={},env={})['supported'])
            self.assertEqual(run.call_count,2)

    def test_missing_sdk_messages_use_existing_translated_status_labels(self):
        translate=lambda value:{'Files missing':'Dateien fehlen','Check failed':'Prüfung fehlgeschlagen'}[value]
        self.assertEqual(sdk.message(dict(supported=False,reason='files'),translate),'Dateien fehlen: SDK 2.4')
        self.assertEqual(sdk.message(dict(supported=False,reason='runtime'),translate),'Prüfung fehlgeschlagen: SDK 2.4')

if __name__=='__main__':unittest.main()
