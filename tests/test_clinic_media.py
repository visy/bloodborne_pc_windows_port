"""tools/discord/clinic_media.py: image shrink, video stills, zip scan."""
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
D = ROOT / "tools" / "discord"


def load(name):
    spec = importlib.util.spec_from_file_location(name, D / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(D))
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.path.remove(str(D))
    return mod


cm = load("clinic_media")
Image = pytest.importorskip("PIL.Image")


def png_bytes(w, h, noise=True):
    raw = os.urandom(w * h * 3) if noise else bytes(w * h * 3)
    im = Image.frombytes("RGB", (w, h), raw)
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return buf.getvalue()


def test_shrink_small_image_untouched():
    data = png_bytes(64, 64)
    assert cm.shrink_image(data) == (data, "orig")
    assert cm.shrink_image(b"not an image" * 400000) == (b"not an image" * 400000, "orig")


def test_shrink_big_image_webp():
    data = png_bytes(1200, 900)
    assert len(data) > 2 * 1024 * 1024
    out, codec = cm.shrink_image(data)
    assert codec in ("webp85", "jpeg85") and len(out) < len(data)
    with Image.open(io.BytesIO(out)) as im:
        assert im.size == (1200, 900)


def test_shrink_caps_side():
    data = png_bytes(3000, 700)
    out, codec = cm.shrink_image(data)
    assert codec != "orig"
    with Image.open(io.BytesIO(out)) as im:
        assert max(im.size) == 2560


@pytest.mark.skipif(not (shutil.which("ffmpeg") and shutil.which("ffprobe")), reason="ffmpeg not installed")
def test_video_stills_count(tmp_path):
    def make(seconds):
        p = tmp_path / f"t{seconds}.mp4"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                        f"testsrc=duration={seconds}:size=640x360:rate=5", "-pix_fmt", "yuv420p", str(p)],
                       check=True)
        return p
    short = cm.video_stills(make(10))
    assert [t for t, _ in short] == [0.0, 4.0, 8.0]
    with Image.open(io.BytesIO(short[0][1])) as im:
        assert im.size[0] == 1280
    assert len(cm.video_stills(make(400))) == 48


def test_zip_listing_and_inner_logs(tmp_path):
    z = tmp_path / "saves.zip"
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("dir/last_session.log", "hello")
        f.writestr("a.png", "x")
        f.writestr("userdata0010", "u")
    assert sorted(cm.archive_listing(z)) == ["a.png", "dir/last_session.log", "userdata0010"]
    inner = dict(cm.archive_inner_logs(z))
    assert inner == {"dir/last_session.log": b"hello", "userdata0010": b"u"}
    bad = tmp_path / "bad.zip"
    bad.write_bytes(b"nope")
    assert cm.archive_listing(bad) is None
    assert cm.archive_listing(tmp_path / "x.7z") is None
    assert cm.free_disk(tmp_path / "missing" / "dir") > 0


def test_enough_disk(tmp_path):
    free = cm.free_disk(tmp_path)
    assert cm.enough_disk(tmp_path, 0, reserve=0)
    assert not cm.enough_disk(tmp_path, free, reserve=1_500_000_000)
    assert not cm.enough_disk(tmp_path, free + 10**9, reserve=0)
