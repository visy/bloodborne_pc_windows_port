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



# --- rar / 7z / nested / guards ---------------------------------------------------------------------------

def zip_bytes(members):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for n, d in members.items():
            z.writestr(n, d)
    return buf.getvalue()


def test_rar_listing_and_inner(tmp_path, monkeypatch):
    unrar = shutil.which("unrar")
    if not unrar:
        pytest.skip("no unrar on PATH")
    rar = shutil.which("rar")
    if rar:
        src = tmp_path / "src"
        src.mkdir()
        (src / "last_session.log").write_text("hello rar")
        (src / "pic.png").write_bytes(b"x")
        arc = tmp_path / "logs.rar"
        subprocess.run([rar, "a", "-ep", str(arc), str(src / "last_session.log"), str(src / "pic.png")],
                       check=True, capture_output=True)
        assert sorted(cm.archive_listing(arc)) == ["last_session.log", "pic.png"]
        assert dict(cm.archive_inner_logs(arc)) == {"last_session.log": b"hello rar"}
        return
    # no `rar` to build a fixture: the unrar commands are mocked (arguments checked, never a shell string)
    arc = tmp_path / "logs.rar"
    arc.write_bytes(b"Rar!\x1a\x07\x01\x00fake")
    calls = []

    def fake_run(cmd, timeout=120):
        calls.append((cmd, timeout))
        return subprocess.CompletedProcess(cmd, 0, b"dir/last_session.log\r\npic.png\r\n", b"")

    class FakePopen:
        def __init__(self, cmd, **kw):
            calls.append((cmd, None))
            assert kw.get("stdout") == subprocess.PIPE and not kw.get("shell")
            self.stdout = io.BytesIO(b"hello rar")
            self.returncode = 0

        def wait(self, timeout=None):
            return 0

        def kill(self):
            pass

    monkeypatch.setattr(cm, "_run", fake_run)
    monkeypatch.setattr(cm.subprocess, "Popen", FakePopen)
    assert cm.archive_listing(arc) == ["dir/last_session.log", "pic.png"]
    assert dict(cm.archive_inner_logs(arc)) == {"dir/last_session.log": b"hello rar"}
    assert calls[0][0][1:4] == ["lb", "-p-", "--"] and calls[0][1] == 120
    reads = [c[0] for c in calls if c[0][1] == "p"]
    assert reads and reads[0][1:4] == ["p", "-inul", "-p-"] and reads[0][-1] == "dir/last_session.log"
    assert all(isinstance(c[0], list) for c in calls)
    # unrar failing (password, broken) -> unreadable
    monkeypatch.setattr(cm, "_run", lambda cmd, timeout=120: subprocess.CompletedProcess(cmd, 3, b"", b"bad"))
    assert cm.archive_listing(arc) is None


def test_7z_inner(tmp_path):
    py7zr = pytest.importorskip("py7zr")
    arc = tmp_path / "logs.7z"
    with py7zr.SevenZipFile(arc, "w") as z:
        z.writestr(b"hello 7z", "dir/last_session.log")
        z.writestr(b"x", "pic.png")
    assert sorted(cm.archive_listing(arc)) == ["dir/last_session.log", "pic.png"]
    assert dict(cm.archive_inner_logs(arc)) == {"dir/last_session.log": b"hello 7z"}
    # 7z inside a zip
    outer = tmp_path / "outer.zip"
    outer.write_bytes(zip_bytes({"readme.txt": b"r", "logs.7z": arc.read_bytes()}))
    assert dict(cm.archive_inner_logs(outer)) == {"readme.txt": b"r", "logs.7z/dir/last_session.log": b"hello 7z"}
    # password protected -> unreadable
    pw = tmp_path / "pw.7z"
    with py7zr.SevenZipFile(pw, "w", password="secret") as z:
        z.writestr(b"hidden", "a.log")
    assert cm.archive_listing(pw) is None


def test_7z_without_py7zr_is_unreadable(tmp_path, monkeypatch):
    arc = tmp_path / "x.7z"
    arc.write_bytes(b"7z\xbc\xaf\x27\x1c" + b"\0" * 40)
    monkeypatch.setitem(sys.modules, "py7zr", None)  # import raises ImportError
    assert cm.archive_listing(arc) is None


def test_7z_one_bad_member_keeps_the_others(tmp_path, monkeypatch):
    """A CRC error in one member fails py7zr's batch read; the other members are still read one by one."""
    import io
    import types

    class FakeArchive:
        def __init__(self, path, **kw):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self, targets):
            if len(targets) > 1 or targets == ["bad.log"]:
                raise ValueError("CRC error")
            return {targets[0]: io.BytesIO(targets[0].encode())}

    monkeypatch.setitem(sys.modules, "py7zr", types.SimpleNamespace(SevenZipFile=FakeArchive))
    monkeypatch.setattr(cm, "_sevenzip_exe", lambda: None)  # the py7zr path
    got = cm._read(tmp_path / "x.7z", "7z", [("a.log", 5), ("bad.log", 7), ("b.log", 5)], 100)
    assert got == {"a.log": b"a.log", "b.log": b"b.log"}


def test_7z_program_skips_damaged_member(tmp_path, monkeypatch):
    """With the 7-Zip program, every good member is read and the ones it reports as CRC failed are left out."""
    import subprocess

    def fake_run(cmd, timeout=120):
        out = Path(next(a for a in cmd if a.startswith("-o"))[2:])
        for name, data in {"logs/a.log": b"aa", "logs/bad.log": b"??", "logs/b.log": b"bb"}.items():
            (out / name).parent.mkdir(parents=True, exist_ok=True)
            (out / name).write_bytes(data)
        return subprocess.CompletedProcess(cmd, 2, b"ERROR: CRC Failed : logs/bad.log\n", b"")

    monkeypatch.setattr(cm, "_run", fake_run)
    got = cm._sevenzip_read("7z", tmp_path / "x.7z", ["logs/a.log", "logs/bad.log", "logs/b.log", "../evil"], 100)
    assert got == {"logs/a.log": b"aa", "logs/b.log": b"bb"}


def test_nested_zip_in_zip(tmp_path):
    l2 = zip_bytes({"deep.log": b"deep"})
    l1 = zip_bytes({"mid.log": b"mid", "l2.zip": l2})
    top = tmp_path / "top.zip"
    top.write_bytes(zip_bytes({"top.log": b"top", "l1.zip": l1, "a.png": b"x"}))
    assert dict(cm.archive_inner_logs(top)) == {"top.log": b"top", "l1.zip/mid.log": b"mid",
                                                "l1.zip/l2.zip/deep.log": b"deep"}
    names = cm.archive_listing(top)
    assert "l1.zip/l2.zip/deep.log" in names and "l1.zip" in names
    # one level deeper than we open: unreadable (never silently dropped)
    l3 = zip_bytes({"x.log": b"x", "l3.zip": zip_bytes({"y.log": b"y"})})
    deep = tmp_path / "deep.zip"
    deep.write_bytes(zip_bytes({"l1.zip": zip_bytes({"l2.zip": l3})}))
    assert cm.archive_listing(deep) is None
    assert dict(cm.archive_inner_logs(deep)) == {"l1.zip/l2.zip/x.log": b"x"}
    # a broken inner archive makes the listing unreadable, the readable logs are still taken
    mixed = tmp_path / "mixed.zip"
    mixed.write_bytes(zip_bytes({"ok.log": b"ok", "bad.zip": b"not an archive"}))
    assert cm.archive_listing(mixed) is None
    assert dict(cm.archive_inner_logs(mixed)) == {"ok.log": b"ok"}


def test_traversal_names_sanitized(tmp_path):
    z = tmp_path / "evil.zip"
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("../../escape.log", "e1")
        f.writestr("/abs/root.log", "e2")
        f.writestr("C:\\win\\drive.log", "e3")
        f.writestr("a\\..\\b.log", "e4")
    inner = dict(cm.archive_inner_logs(z))
    assert set(inner) == {"escape.log", "abs/root.log", "win/drive.log", "a/b.log"}
    for n in cm.archive_listing(z):
        assert ".." not in n.split("/") and not n.startswith("/") and ":" not in n
    assert not (tmp_path.parent / "escape.log").exists() and not (tmp_path / "escape.log").exists()
    # a nested archive named with traversal is written under our own temp name
    top = tmp_path / "top.zip"
    top.write_bytes(zip_bytes({"../../in.zip": zip_bytes({"../x.log": b"x"})}))
    assert dict(cm.archive_inner_logs(top)) == {"in.zip/x.log": b"x"}
    assert cm._clean("..") is None and cm._clean("") is None


def test_bomb_guard(tmp_path, monkeypatch):
    monkeypatch.setattr(cm, "MAX_FILE_BYTES", 1000)
    monkeypatch.setattr(cm, "MAX_TOTAL_BYTES", 2500)
    big = tmp_path / "big.zip"
    big.write_bytes(zip_bytes({"huge.log": b"a" * 5000, "small.log": b"b"}))
    assert dict(cm.archive_inner_logs(big)) == {"small.log": b"b"}  # one oversized file is skipped
    tot = tmp_path / "tot.zip"
    tot.write_bytes(zip_bytes({f"f{i}.log": b"c" * 900 for i in range(5)}))
    assert cm.archive_listing(tot) is None  # total over the cap: unreadable
    assert cm.archive_inner_logs(tot) == []  # declared total over the cap: nothing is extracted
    monkeypatch.setattr(cm, "MAX_MEMBERS", 10)
    many = tmp_path / "many.zip"
    many.write_bytes(zip_bytes({f"m{i}.png": b"" for i in range(11)}))
    assert cm.archive_listing(many) is None and cm.archive_inner_logs(many) == []


def test_scan_removes_its_temp_dir(tmp_path, monkeypatch):
    seen = []
    real = cm.tempfile.TemporaryDirectory

    def spy(*a, **k):
        d = real(*a, **k)
        seen.append(d.name)
        return d
    monkeypatch.setattr(cm.tempfile, "TemporaryDirectory", spy)
    z = tmp_path / "n.zip"
    z.write_bytes(zip_bytes({"in.zip": zip_bytes({"a.log": b"a"})}))
    cm.archive_inner_logs(z)
    assert seen and not any(Path(d).exists() for d in seen)


def test_encrypted_zip_is_unreadable(tmp_path):
    z = tmp_path / "enc.zip"
    data = bytearray(zip_bytes({"a.log": b"secret"}))
    for sig, off in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):  # the "encrypted" flag, local + central header
        data[data.find(sig) + off] |= 1
    z.write_bytes(bytes(data))
    assert cm.archive_listing(z) is None
