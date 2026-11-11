"""Writes patches/chalice_glyphs_community.json: the shipped snapshot of Noxde's community Chalice glyph list
(https://gist.github.com/Noxde/a29f699f4175bf315d9bd4baeebefb66), the fallback the game reads until the launcher has
fetched a newer one into <user dir>/community/chalice_glyphs.json.

    python tools/glyphdb_snapshot.py            # fetch the gist from GitHub
    python tools/glyphdb_snapshot.py gist.json  # or read a saved copy of the gist API reply

The same strict validation as the launcher (launcher.glyphdb_from_gist); the creators' names are zeroed out of every record.
"""
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "patches" / "chalice_glyphs_community.json"


def main(argv):
    spec = importlib.util.spec_from_file_location("bb_launcher_snapshot", ROOT / "launcher.py")
    launcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(launcher)
    text = Path(argv[1]).read_text(encoding="utf-8") if len(argv) > 1 else launcher.glyphdb_fetch()
    cache, dropped = launcher.glyphdb_from_gist(text)
    if not cache or not cache["glyphs"]:
        print("nothing usable in the gist reply")
        return 1
    OUT.write_text(json.dumps(cache, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(cache['glyphs'])} glyphs written to {OUT} ({dropped} dropped)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
