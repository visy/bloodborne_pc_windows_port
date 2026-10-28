"""Iosefka's clinic: pictures, video stills, zip contents. Pillow + the ffmpeg CLI (2.8 compatible)."""
import io
import os
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path

try:
    import clinic_logic as _logic
except ImportError:  # loaded by path without tools/discord on sys.path
    import importlib.util
    _spec = importlib.util.spec_from_file_location("clinic_logic", Path(__file__).with_name("clinic_logic.py"))
    _logic = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_logic)

IMAGE_LIMIT = 2 * 1024 * 1024
IMAGE_MAX_SIDE = 2560


def _webp_ok() -> bool:
    try:
        from PIL import features
        return bool(features.check("webp"))
    except Exception:
        return False


def _encode(im, webp_q: int, jpeg_q: int) -> tuple[bytes, str]:
    """Pillow image -> (bytes, "webp"|"jpeg"); WebP when the build has it."""
    buf = io.BytesIO()
    if _webp_ok():
        im.save(buf, "WEBP", quality=webp_q, method=4)
        return buf.getvalue(), "webp"
    if im.mode not in ("RGB", "L"):
        im = im.convert("RGB")
    im.save(buf, "JPEG", quality=jpeg_q, optimize=True)
    return buf.getvalue(), "jpeg"


def shrink_image(data: bytes) -> tuple[bytes, str]:
    """(stored bytes, codec "orig"|"webp85"|"jpeg85"). Unchanged when <= 2 MB, animated, unreadable, or
    when re-encoding does not make it smaller. The caller keys the file by the original sha256."""
    if len(data) <= IMAGE_LIMIT:
        return data, "orig"
    try:
        from PIL import Image
        im = Image.open(io.BytesIO(data))
        if getattr(im, "is_animated", False):
            return data, "orig"
        im.load()
        if max(im.size) > IMAGE_MAX_SIDE:
            im.thumbnail((IMAGE_MAX_SIDE, IMAGE_MAX_SIDE))
        out, kind = _encode(im, 85, 85)
    except Exception:
        return data, "orig"
    if len(out) >= len(data):
        return data, "orig"
    return out, kind + "85"


def _run(cmd: list[str], timeout: int = 120) -> subprocess.CompletedProcess:
    if os.name == "posix" and shutil.which("nice"):
        cmd = ["nice", "-n", "10"] + cmd
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)


def _duration(path: Path) -> float:
    r = _run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
              "-of", "default=noprint_wrappers=1:nokey=1", str(path)], timeout=60)
    try:
        return float(r.stdout.decode().strip().splitlines()[0])
    except (ValueError, IndexError):
        return 0.0


def video_stills(path: Path, every_s=4.0, max_frames=48, width=1280) -> list[tuple[float, bytes]]:
    """(seconds, image bytes) one frame per `every_s`, at most `max_frames` evenly spread. [] when ffmpeg
    or the file is unusable. One ffmpeg process at a time (the caller serialises)."""
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        return []
    dur = _duration(Path(path))
    if dur <= 0:
        return []
    count = max(1, int(dur // every_s) + (1 if dur % every_s > 0.5 else 0)) if dur > every_s else 1
    if count > max_frames:
        times = [i * dur / max_frames for i in range(max_frames)]
    else:
        times = [i * every_s for i in range(count)]
    from PIL import Image
    frames = []
    with tempfile.TemporaryDirectory(prefix="stills_") as tmp:
        for n, t in enumerate(times):
            png = Path(tmp) / f"f{n:03d}.png"
            r = _run(["ffmpeg", "-v", "error", "-y", "-ss", f"{t:.2f}", "-i", str(path), "-an",
                      "-frames:v", "1", "-vf", f"scale={width}:-2", str(png)])
            if r.returncode != 0 or not png.is_file() or png.stat().st_size == 0:
                continue
            with Image.open(png) as im:
                im.load()
                data, _ = _encode(im.convert("RGB"), 60, 70)
            frames.append((round(t, 2), data))
    return frames


# --- archives: zip (zipfile), rar (the `unrar` binary), 7z (py7zr, imported lazily) --------------------------

MAX_FILE_BYTES = 200 * 1024 * 1024    # one extracted inner file
MAX_TOTAL_BYTES = 500 * 1024 * 1024   # everything extracted from one upload
MAX_MEMBERS = 2000
MAX_DEPTH = 2                         # archives inside archives: how many levels are opened
UNRAR_TIMEOUT = 120


def _sniff(path: Path) -> str | None:
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return None
    if head.startswith(b"Rar!\x1a\x07"):
        return "rar"
    if head.startswith(b"7z\xbc\xaf\x27\x1c"):
        return "7z"
    try:
        return "zip" if zipfile.is_zipfile(path) else None
    except OSError:
        return None


def _clean(name: str) -> str | None:
    """A member name made safe: forward slashes, no drive, no empty/`.`/`..` parts. None when nothing is left."""
    parts = []
    for p in (name or "").replace("\\", "/").split("/"):
        if p in ("", ".", "..") or p.endswith(":") or "\x00" in p:
            continue
        parts.append(p.strip())
    parts = [p for p in parts if p]
    return "/".join(parts) if parts else None


def _unrar_list(path: Path) -> list[tuple[str, int | None]] | None:
    exe = shutil.which("unrar")
    if not exe:
        return None
    try:
        r = _run([exe, "lb", "-p-", "--", str(path)], timeout=UNRAR_TIMEOUT)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    names = [ln.rstrip("\r") for ln in r.stdout.decode("utf-8", "replace").split("\n")]
    return [(n, None) for n in names if n]


def _unrar_read(path: Path, member: str, limit: int) -> bytes | None:
    exe = shutil.which("unrar")
    if not exe or any(c in member for c in "*?") or member.startswith("-"):
        return None
    try:
        p = subprocess.Popen([exe, "p", "-inul", "-p-", "--", str(path), member],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except OSError:
        return None
    try:
        data = p.stdout.read(limit + 1)
        if len(data) > limit:
            p.kill()
            return None
        p.wait(timeout=UNRAR_TIMEOUT)
    except (subprocess.TimeoutExpired, OSError):
        p.kill()
        return None
    finally:
        p.stdout.close()
        try:
            p.wait(timeout=5)
        except subprocess.SubprocessError:
            p.kill()
    return data if p.returncode == 0 else None


def _sevenzip_exe() -> str | None:
    """The 7-Zip program (7z / 7zz / 7za), preferred over py7zr: it reads the good members of an archive with a
    damaged one (py7zr gives up on the whole read) and is far lighter on a small server."""
    for name in ("7z", "7zz", "7za"):
        exe = shutil.which(name)
        if exe:
            return exe
    return None


def _sevenzip_read(exe: str, path: Path, members: list[str], limit: int) -> dict[str, bytes] | None:
    """Extract `members` with the 7-Zip program into a private temp folder; members it reports as damaged (CRC
    failed / data error) or larger than `limit` are left out. None when 7-Zip could not run at all."""
    if any(m.startswith("-") or any(c in m for c in "*?") for m in members):
        return None
    out: dict[str, bytes] = {}
    with tempfile.TemporaryDirectory(prefix="7z_") as tmp:
        try:
            # no -bb1 etc.: Ubuntu's p7zip 9.20 rejects switches newer than 2010 ("Incorrect command line")
            r = _run([exe, "x", "-y", "-p-", f"-o{tmp}", "--", str(path), *members], timeout=UNRAR_TIMEOUT)
        except (OSError, subprocess.SubprocessError):
            return None
        text = (r.stdout + (r.stderr or b"")).decode("utf-8", "replace") if isinstance(r.stdout, bytes) else ""
        bad = {ln.rsplit(" : ", 1)[-1].strip() for ln in text.splitlines()
               if ("CRC Failed" in ln or "Data Error" in ln) and " : " in ln}
        root = Path(tmp).resolve()
        for m in members:
            if m in bad:
                continue
            f = (root / m).resolve()
            if root not in f.parents or not f.is_file() or f.stat().st_size > limit:
                continue
            out[m] = f.read_bytes()
    return out


def _list(path: Path, fmt: str) -> list[tuple[str, int | None]] | None:
    """[(raw member name, size or None)] of one archive level; None when unreadable or password protected."""
    try:
        if fmt == "zip":
            with zipfile.ZipFile(path) as z:
                infos = [i for i in z.infolist() if not i.is_dir()]
                if any(i.flag_bits & 0x1 for i in infos):
                    return None
                return [(i.filename, i.file_size) for i in infos]
        if fmt == "rar":
            return _unrar_list(path)
        if fmt == "7z":
            try:
                import py7zr
            except ImportError:
                return None
            with py7zr.SevenZipFile(path) as z:
                if z.needs_password():
                    return None
                return [(i.filename, i.uncompressed) for i in z.list() if not i.is_directory]
    except Exception:
        return None
    return None


def _read(path: Path, fmt: str, members: list[tuple[str, int | None]], limit: int) -> dict[str, bytes] | None:
    """Bytes of `members` (each <= limit). Members that cannot be read are left out; None: the archive broke."""
    out: dict[str, bytes] = {}
    try:
        if fmt == "zip":
            with zipfile.ZipFile(path) as z:
                for name, _ in members:
                    with z.open(name) as f:
                        data = f.read(limit + 1)
                    if len(data) <= limit:
                        out[name] = data
        elif fmt == "rar":
            for name, _ in members:
                data = _unrar_read(path, name, limit)
                if data is not None:
                    out[name] = data
        elif fmt == "7z":
            exe = _sevenzip_exe()
            if exe:
                return _sevenzip_read(exe, path, [n for n, _ in members], limit)
            import py7zr
            try:  # mp=False: py7zr's parallel decoding misreports CRCs on a small server
                with py7zr.SevenZipFile(path, mp=False) as z:
                    got = z.read([n for n, _ in members]) or {}
            except Exception:
                # one bad member (e.g. a CRC error) fails the whole batch: read them one by one instead and
                # leave out only the ones that fail
                got = {}
                for name, _ in members:
                    try:
                        with py7zr.SevenZipFile(path, mp=False) as z:
                            got.update(z.read([name]) or {})
                    except Exception:
                        pass
            for name, bio in got.items():
                data = bio.read(limit + 1)
                if len(data) <= limit:
                    out[name] = data
    except Exception:
        return None
    return out


class _Scan:
    def __init__(self, tmp: Path):
        self.tmp, self.n_members, self.total, self.ok, self.seq = tmp, 0, 0, True, 0
        self.names: list[str] = []
        self.logs: list[tuple[str, bytes]] = []


def _scan_level(path: Path, prefix: str, depth: int, st: _Scan) -> None:
    fmt = _sniff(path)
    members = _list(path, fmt) if fmt else None
    if members is None:
        st.ok = False
        return
    st.n_members += len(members)
    if st.n_members > MAX_MEMBERS:
        st.ok = False
        return
    want, seen = [], set()
    for raw, size in members:
        clean = _clean(raw)
        if clean is None or raw in seen:
            continue
        seen.add(raw)
        full = prefix + clean
        st.names.append(full)
        kind = _logic.file_kind(clean)
        if kind in _logic.REMOVABLE_KINDS or kind == "archive":
            if size is not None and size > MAX_FILE_BYTES:
                if kind == "archive":
                    st.ok = False  # cannot look inside: unreadable
                continue
            want.append((raw, size, full, kind))
    if not want:
        return
    declared = sum(s or 0 for _, s, _, _ in want)
    if st.total + declared > MAX_TOTAL_BYTES:
        st.ok = False
        return
    got = _read(path, fmt, [(r, s) for r, s, _, _ in want], MAX_FILE_BYTES)
    if got is None:
        st.ok = False
        return
    for raw, _size, full, kind in want:
        data = got.get(raw)
        if data is None:
            if kind == "archive":
                st.ok = False
            continue
        st.total += len(data)
        if st.total > MAX_TOTAL_BYTES:
            st.ok = False
            return
        if kind != "archive":
            st.logs.append((full, data))
            continue
        if depth >= MAX_DEPTH:
            st.ok = False  # nested deeper than we open: unreadable
            continue
        st.seq += 1
        inner = st.tmp / f"n{st.seq}.bin"  # our own name, never the member's
        inner.write_bytes(data)
        del data
        try:
            _scan_level(inner, full + "/", depth + 1, st)
        finally:
            inner.unlink(missing_ok=True)
        if st.n_members > MAX_MEMBERS or st.total > MAX_TOTAL_BYTES:
            return


def _scan(path: Path) -> _Scan:
    with tempfile.TemporaryDirectory(prefix="arch_") as tmp:
        st = _Scan(Path(tmp))
        try:
            _scan_level(Path(path), "", 0, st)
        except Exception:
            st.ok = False
    return st


def archive_listing(path: Path) -> list[str] | None:
    """Member names of a zip/rar/7z (nested archives' members as `inner.zip/name`, down to depth 2); None when
    the archive, or one inside it, is unreadable, password protected, too big or too deep (counted as removable)."""
    st = _scan(path)
    return st.names if st.ok else None


def archive_inner_logs(path: Path) -> list[tuple[str, bytes]]:
    """(member name, bytes) of members that are removable kinds (logs, dumps, game files), nested archives
    included. Whatever could be read is returned even when the archive is only partly readable."""
    return _scan(path).logs


def free_disk(path: Path) -> int:
    p = Path(path)
    while not p.exists() and p != p.parent:
        p = p.parent
    return shutil.disk_usage(p).free


def enough_disk(path: Path, need_bytes: int, reserve=1_500_000_000) -> bool:
    """True when `need_bytes` fit and `reserve` stays free. Callers check before each download."""
    return free_disk(path) >= need_bytes + reserve
