"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))

for candidate in ['bb-probe.exe', 'bbport.exe', 'bb-probe', 'bbport']:
    p = ROOT / 'out' / candidate
    if p.exists():
        EXE = p
        break
else:
    EXE = ROOT / 'out' / 'bbport.exe'


def test_exe(name):
    """out/<name> of a native test program (out/<name>.exe on Windows)."""
    return ROOT / 'out' / (name + '.exe' if os.name == 'nt' else name)


def build_hint(name):
    """The skip reason when out/<name> has not been built."""
    if os.name == 'nt':
        return f'out/{name}.exe not built: cmake --build out/gpu --target {name}'
    return f'out/{name} not built: bash build.sh --test'
