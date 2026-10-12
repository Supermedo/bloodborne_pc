"""Full Windows loader/SDL/swapchain/cache startup with a synthetic guest image."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from test_probe import package


with tempfile.TemporaryDirectory(prefix='bbport GPU проверка ') as directory:
    work = Path(directory)
    image = work / 'boot.bin'
    image.write_bytes(package(b'\xff\x25\x02\0\0\0\x90\x90', [(8, 1, 0, 0)], ['fixture-gpu-smoke']))
    env = {**os.environ, 'BB_HIDDEN_WINDOW': '1', 'BB_FULLSCREEN': '0',
           'BB_GPU_USER_DIR': str(work / 'user'), 'BB_CONFIG': str(work / 'bbport.ini'),
           'BB_UPSCALER': 'fsr3'}
    result = subprocess.run([sys.argv[1], str(image), '--user', str(work / 'user'), '--timeout', '30'],
                            env=env, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
    print(result.stdout)
    print(result.stderr, file=sys.stderr)
    if result.returncode != 20 or 'GPU: window and Vulkan presenter ready' not in result.stdout:
        raise SystemExit(f'GPU startup failed: exit {result.returncode}')
    if 'first unsupported PS4 import: fixture-gpu-smoke' not in result.stdout:
        raise SystemExit('Synthetic guest did not reach its terminal import')
    print('PASS: Windows loader, hidden SDL surface, presenter and GPU caches')
