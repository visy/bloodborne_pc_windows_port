#!/usr/bin/env python3
"""The boot image the runtime loads (out/boot-linked.bin), built once and then reused.

Runs prepare.py (eboot.bin -> memory image, game check), link_libc.py and link_modules.py in one
process, and only when something they read changed: the game's eboot.bin, sce_module/*.prx,
sce_sys/param.sfo and shader folder (the game check reads it), these scripts themselves, and
BB_SKIP_GAME_CHECK. The key is the files' sizes and modification times (no hashing: an unchanged
launch costs a few stat calls). The outputs are the same files as before (boot.bin, eboot.elf,
entry.bin, analysis.json, boot-libc.bin, libc-link.json, boot-linked.bin, link.json).

  boot_image.py GAME --out OUT [--cache-dir DIR]

out/boot_image.json records the key and the lines the steps printed (shown again on a reuse).
--cache-dir (or BB_BOOT_CACHE_DIR): a store shared by several out folders (the test instances of
tools/mp/instances.py): DIR/<key>/ holds the files, hard-linked into each out folder. BB_BOOT_CACHE=0
always rebuilds. Exit codes as prepare.py: 2 = unsupported game files, 1 = other errors.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import cpu_compat  # noqa: E402

FORMAT = 1
OUTPUTS = ('boot.bin', 'eboot.elf', 'entry.bin', 'analysis.json', 'boot-libc.bin', 'libc-link.json',
           'boot-linked.bin', 'link.json')
SCRIPTS = ('boot_image.py', 'prepare.py', 'game_check.py', 'link_libc.py', 'link_modules.py',
           'cpu_compat.py')
# Data the scripts read into the image (link_modules.py: the red-zone store sites): a build that
# changes only these must not reuse an image linked with the old ones.
DATA = (HERE.parent / 'patches' / 'redzone.json', HERE.parent / 'tools' / 'legacy-cpu-plan.json')
STAMP = 'boot_image.json'


def _stat(path: Path):
    try:
        st = path.stat()
        return [st.st_size, st.st_mtime_ns]
    except OSError:
        return None


def _folder(path: Path):
    """(relative name, size, mtime) of every file below path (os.scandir: no stat per file)."""
    found = []

    def walk(folder, rel):
        try:
            entries = list(os.scandir(folder))
        except OSError:
            return
        for e in sorted(entries, key=lambda e: e.name):
            if e.is_dir(follow_symlinks=False):
                walk(e.path, rel + e.name + '/')
            elif e.is_file():
                st = e.stat()
                found.append([rel + e.name, st.st_size, st.st_mtime_ns])

    walk(path, '')
    return found


def key(game: Path):
    game = Path(game)
    parts = {
        'format': FORMAT,
        'platform': sys.platform,
        'game': str(game.resolve()).lower() if os.name == 'nt' else str(game.resolve()),
        'eboot': _stat(game / 'eboot.bin'),
        'param': _stat(game / 'sce_sys' / 'param.sfo'),
        'modules': _folder(game / 'sce_module'),
        'shaders': _folder(game / 'dvdroot_ps4' / 'shader'),
        'scripts': {n: hashlib.sha256((HERE / n).read_bytes()).hexdigest()[:16] for n in SCRIPTS},
        'data': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()[:16] if p.is_file() else None for p in DATA},
        'legacy_cpu': cpu_compat.enabled(),   # the Legacy CPU translation on/off (BB_LEGACY_CPU, bbport.ini, CPUID)
        'skip_check': os.environ.get('BB_SKIP_GAME_CHECK') == '1',
    }
    return hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()[:24]


def _complete(folder: Path, sizes: dict):
    return all(_stat(folder / n) and _stat(folder / n)[0] == sizes.get(n) for n in OUTPUTS)


def patiently(operation, *args, seconds=10.0):
    """operation(*args), retried while Windows refuses it (WinError 5/32): an antivirus or the
    search indexer scanning a fresh file holds it for a moment."""
    deadline = time.monotonic() + seconds
    delay = 0.05
    while True:
        try:
            return operation(*args)
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(delay)
            delay = min(delay * 2, 0.5)


def _unlink(path: Path):
    with contextlib.suppress(FileNotFoundError):
        patiently(path.unlink)


def _link(src: Path, dst: Path):
    tmp = dst.with_name(dst.name + '.linking')
    with contextlib.suppress(OSError):
        tmp.unlink()
    try:
        os.link(src, tmp)
    except OSError:
        shutil.copy2(src, tmp)
    patiently(os.replace, tmp, dst)


def _read_stamp(path: Path):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def build(game: Path, out: Path):
    """The three steps in this process; returns the lines they printed."""
    import prepare
    import link_libc
    import link_modules
    out.mkdir(parents=True, exist_ok=True)
    # Never write through a hard link into the shared store: remove the old outputs first.
    for n in OUTPUTS:
        _unlink(out / n)
    buf = io.StringIO()

    class Tee(io.TextIOBase):
        def write(self, s):
            sys.__stdout__.write(s)
            buf.write(s)
            return len(s)

        def flush(self):
            sys.__stdout__.flush()

    with contextlib.redirect_stdout(Tee()):
        prepare.prepare(game, out)
        link_libc.link(game, out)
        link_modules.link(game, out, link_modules.DEFAULT_MODULES)
    return buf.getvalue().splitlines()


def write_compat(out: Path):
    """out/cpu_compat.txt: '1' when the image holds the Legacy CPU translations, else empty. run.bat
    and run.sh export BB_CPU_COMPAT=1 from it, which bb-probe's CPU check reads."""
    try:
        enabled = bool(json.loads((out / 'link.json').read_text(encoding='utf-8')).get('cpu_compat', {}).get('enabled'))
    except (OSError, ValueError):
        enabled = False
    (out / 'cpu_compat.txt').write_text('1\n' if enabled else '\n', encoding='ascii')
    print('Legacy CPU translation: ' + ('on (this image runs without BMI1, LZCNT, MOVBE)' if enabled else
          'off (the CPU has the instructions, or legacy_cpu=0)'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('game', type=Path)
    ap.add_argument('--out', type=Path, default=HERE.parent / 'out')
    ap.add_argument('--cache-dir', type=Path, default=os.environ.get('BB_BOOT_CACHE_DIR') or None)
    ap.add_argument('--sku', choices=['full', 'trial'],
                    help='also write the AppContent profile (content_profile.py) for this SKU')
    a = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(errors='replace')  # the title has a (TM); consoles in GBK etc.
    import prepare
    try:
        image(a)
        write_compat(a.out)
        if a.sku:
            import content_profile
            content_profile.prepare(a.game, a.out, a.sku)
    except prepare.GameCheckError as error:
        ap.exit(2, f'\nUnsupported game files: {error}\nSet BB_SKIP_GAME_CHECK=1 to start anyway.\n')
    except (ValueError, OSError, StopIteration, KeyError, IndexError) as error:
        ap.exit(1, f'boot image failed: {error}\n')


def image(a):
    """out's boot image: reused, linked from the shared store, or built."""
    t0 = time.time()
    k = key(a.game)
    reuse = os.environ.get('BB_BOOT_CACHE', '1') != '0'
    stamp = _read_stamp(a.out / STAMP) if reuse else None
    if stamp and stamp.get('key') == k and _complete(a.out, stamp.get('sizes', {})):
        print('\n'.join(stamp.get('lines', [])))
        print(f'Boot image: unchanged, reused ({time.time() - t0:.2f} s; BB_BOOT_CACHE=0 rebuilds)')
        return
    shared = a.cache_dir / k if a.cache_dir and reuse else None
    sstamp = _read_stamp(shared / STAMP) if shared else None
    if sstamp and sstamp.get('key') == k and _complete(shared, sstamp.get('sizes', {})):
        a.out.mkdir(parents=True, exist_ok=True)
        for n in OUTPUTS:
            _link(shared / n, a.out / n)
        (a.out / STAMP).write_text(json.dumps(sstamp, indent=1) + '\n', encoding='utf-8')
        print('\n'.join(sstamp.get('lines', [])))
        print(f'Boot image: linked from {shared} ({time.time() - t0:.2f} s)')
        return
    with contextlib.suppress(OSError):
        (a.out / STAMP).unlink()
    lines = build(a.game, a.out)
    stamp = dict(key=k, sizes={n: _stat(a.out / n)[0] for n in OUTPUTS}, lines=lines, built=time.time())
    (a.out / STAMP).write_text(json.dumps(stamp, indent=1) + '\n', encoding='utf-8')
    print(f'Boot image: built in {time.time() - t0:.1f} s (reused while the game files stay the same)')
    if shared:
        store(a.cache_dir, k, a.out, stamp)


def store(cache_dir: Path, k: str, out: Path, stamp: dict, keep=3):
    """Adds out's files to the shared store as DIR/<k> (built under a temporary name and renamed,
    so a parallel instance never sees half of it); keeps the newest `keep` keys."""
    final = cache_dir / k
    if final.exists():
        return
    tmp = cache_dir / f'.{k}.{os.getpid()}'
    try:
        tmp.mkdir(parents=True, exist_ok=True)
        for n in OUTPUTS:
            _link(out / n, tmp / n)
        (tmp / STAMP).write_text(json.dumps(stamp, indent=1) + '\n', encoding='utf-8')
        os.replace(tmp, final)
    except OSError:
        shutil.rmtree(tmp, ignore_errors=True)
        return
    entries = sorted((p for p in cache_dir.iterdir() if p.is_dir() and not p.name.startswith('.')),
                     key=lambda p: p.stat().st_mtime, reverse=True)
    for old in entries[keep:]:
        shutil.rmtree(old, ignore_errors=True)


if __name__ == '__main__':
    main()
