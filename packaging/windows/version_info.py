"""Generate PyInstaller's Windows version resource from the port's VERSION file."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
version = (root / 'VERSION.txt').read_text(encoding='ascii').strip()
parts = tuple(map(int, version.split('.')))
assert 1 <= len(parts) <= 4 and all(0 <= n <= 65535 for n in parts)
numbers = parts + (0,) * (4 - len(parts))
full = '.'.join(map(str, numbers))
resource = f"""VSVersionInfo(
  ffi=FixedFileInfo(filevers={numbers!r}, prodvers={numbers!r}, mask=0x3f,
                   flags=0, OS=0x40004, fileType=1, subtype=0, date=(0, 0)),
  kids=[StringFileInfo([StringTable('040904b0', [
    StringStruct('FileDescription', 'Bloodborne launcher'),
    StringStruct('FileVersion', '{full}'),
    StringStruct('ProductName', 'Bloodborne (bbport)'),
    StringStruct('ProductVersion', 'v{version}')])]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])])
"""
Path(sys.argv[1]).write_text(resource, encoding='utf-8')
