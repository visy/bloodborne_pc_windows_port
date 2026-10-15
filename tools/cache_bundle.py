#!/usr/bin/env python3
"""Packs a shader cache folder into one .zip to share with other PCs (the launcher's import and
the user\\cache_import drop folder read it). Never commit such a .zip or attach it to a release.

  python tools/cache_bundle.py CACHE_DIR OUT.zip [--build TEXT] [--gpu-name TEXT] [--note TEXT]

CACHE_DIR is a cache root (the folder holding CUSA03173\\), e.g. <game>\\user\\cache. The .zip has:
  CUSA03173\\*.key, *.src                 every GPU: pipeline keys and shader sources
  gpu\\<label>\\CUSA03173\\...             this GPU only: .meta/.spv (built shaders), profile.bin,
                                         the driver's pipeline binaries (.vkp)
  README.txt                             where to put it and what each part does
"""
import argparse, importlib.util, sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_launcher():
    spec = importlib.util.spec_from_file_location("bb_launcher_bundle", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def readme(label: str, gpu_name: str, build: str, note: str, portable: int, gpu: int) -> str:
    gpu_line = f"{label}" + (f" ({gpu_name})" if gpu_name else "")
    return f"""Bloodborne PC port (bbport) - shared shader cache, {date.today().isoformat()}
===================================================================

Made with: {build or "(build not given)"}
GPU part made on: {gpu_line}
Files: {portable} GPU-independent, {gpu} for that GPU only.
{note}
WHERE TO PUT IT
  Easiest: copy this .zip unopened into <game folder>\\user\\cache_import\\ (make the folder if
  it is not there) and start the game from the launcher. The launcher merges it before the game
  starts and renames it to .zip.imported. Or: launcher > Advanced > "Import shader cache..." and
  pick this .zip. Nothing you already have is overwritten.
  (Without the launcher: unzip the CUSA03173 folder into <game folder>\\user\\cache\\, and on the
  same GPU also the CUSA03173 folder inside gpu\\<label>\\.)

WHAT HELPS ON WHICH PC
  CUSA03173\\*.key, *.src - every GPU (NVIDIA, AMD, Intel): the pipelines the game needs and the
    original shader code. On the first start the game translates them for your GPU ("Preparing
    shaders" before the intro, a few minutes once), and from then on these places, enemies and
    effects do not stutter the first time you see them.
  gpu\\{label}\\ - only this GPU with this driver: the shaders already translated (.meta/.spv,
    profile.bin) and the driver's own pipeline binaries (.vkp). Taken only when your GPU matches
    (same GPU features / profile); otherwise left out and the game builds its own from the
    sources. The .vkp is loaded only when its header names the same GPU, device and driver; with
    another driver version it is ignored (harmless) and rebuilt as you play.

BUILD / VERSION
  The cache files are versioned (pipeline key 6, shader source 1 with the Windows build's layout,
  shader binary 9 / meta 7): a port build with the same formats uses every file; a build that
  changed the translated formats rebuilds them from the sources by itself, and one that changed
  the key or source format skips those files (nothing breaks, it builds its own as you play).
  The cache_import folder needs a launcher from this build on; an older launcher's "Import
  shader cache..." takes only the GPU-independent part. The NVIDIA driver also keeps its own
  cache in %LOCALAPPDATA%\\NVIDIA\\DXCache / GLCache; that one is per PC and is not in this .zip.

Private test data: do not upload it to GitHub or attach it to a release.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cache_dir")
    ap.add_argument("out")
    ap.add_argument("--build", default="")
    ap.add_argument("--gpu-name", default="")
    ap.add_argument("--note", default="")
    a = ap.parse_args()
    L = load_launcher()
    root, out = Path(a.cache_dir), Path(a.out)
    if not root.is_dir():
        sys.exit(f"no cache folder: {root}")
    tmp = out.with_suffix(".tmp.zip")
    r = L.export_shader_cache_bundle(tmp, root)  # counts and label first
    text = readme(r["label"], a.gpu_name, a.build, (a.note + "\n") if a.note else "", r["portable"], r["gpu"])
    r = L.export_shader_cache_bundle(out, root, readme=text, label=r["label"])
    tmp.unlink()
    print(f"{out}: {r['portable']} GPU-independent files, {r['gpu']} for {r['label']}, "
          f"{out.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
