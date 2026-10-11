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


test_exe.__test__ = False  # a helper the test modules import, not a pytest test


def build_hint(name):
    """The skip reason when out/<name> has not been built."""
    if os.name == 'nt':
        return f'out/{name}.exe not built: cmake --build out/gpu --target {name}'
    return f'out/{name} not built: bash build.sh --test'


def posix_tool(name):
    """A POSIX tool (bash, dirname, ...) for the run.sh tests. On Windows never System32's
    bash.exe (WSL: run.sh fails there, e.g. when the tests run from PowerShell, where it comes
    first) nor the WindowsApps stubs: the PATH entries first, then Git for Windows and MSYS2.
    None when there is none."""
    import shutil
    if os.name != 'nt':
        return shutil.which(name)
    dirs = [d for d in os.environ.get('PATH', '').split(os.pathsep) if d]
    dirs += [r'C:\Program Files\Git\usr\bin', r'C:\Program Files\Git\bin', r'C:\msys64\usr\bin']
    for d in dirs:
        low = d.lower().rstrip('\\/')
        if low.endswith('system32') or 'windowsapps' in low:
            continue
        found = shutil.which(name, path=d)
        if found:
            return found
    return None


def posix_env(env):
    """`env` for running run.sh: the POSIX tools' folder (sed, grep, ...: posix_tool's bash) first
    in PATH, so a PowerShell PATH without Git's usr\\bin works too."""
    bash = posix_tool('bash')
    if os.name == 'nt' and bash:
        env = dict(env)
        env['PATH'] = str(Path(bash).parent) + os.pathsep + env.get('PATH', '')
    return env
