"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def executable(name):
    """Native test binaries built by build.sh on either host."""
    if sys.platform == 'win32':
        return ROOT / 'out' / 'gpu' / (name + '.exe')
    return ROOT / 'out' / name
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))
