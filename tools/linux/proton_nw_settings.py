#!/usr/bin/env python3
"""Point a Nightwatch Co-op launcher (run under Proton) at a game folder and set its test options.

  python3 proton_nw_settings.py <settings.json> <game-dir (unix path)> [KEY=VALUE ...]

Writes game_dir as a Z: path, fullscreen off, and puts every KEY=VALUE (launcher setting names such as
fps_mode=30, or env:NAME=VALUE for the extra_env list: space separated, so no spaces in values) into the file.
The launcher creates the file on its first start (the prefix's AppData/Roaming/nightwatch/settings.json).
"""
import json, os, sys

BS = chr(92)


def main():
    path, game = sys.argv[1], sys.argv[2]
    try:
        with open(path, encoding="utf-8") as f:
            s = json.load(f)
    except FileNotFoundError:
        s = {}
    s["game_dir"] = "Z:" + game.replace("/", BS)
    s["fullscreen"] = False
    env = []
    for kv in sys.argv[3:]:
        k, v = kv.split("=", 1)
        if k.startswith("env:"):
            env.append(k[4:] + "=" + v)
        else:
            s[k] = v if not v in ("true", "false") else v == "true"
    if env:
        s["extra_env"] = " ".join(env)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)
    print("settings:", path, "game_dir", s["game_dir"], "extra_env", s.get("extra_env", ""))


if __name__ == "__main__":
    main()
