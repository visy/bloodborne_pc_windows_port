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
MAX_INNER_BYTES = 256 * 1024 * 1024


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


def archive_listing(path: Path) -> list[str] | None:
    """Member names of a zip; None for 7z/rar and for anything unreadable (counted as removable)."""
    try:
        with zipfile.ZipFile(path) as z:
            return [i.filename for i in z.infolist() if not i.is_dir()]
    except (zipfile.BadZipFile, OSError, NotImplementedError, RuntimeError):
        return None


def archive_inner_logs(path: Path) -> list[tuple[str, bytes]]:
    """(member name, bytes) of zip members that are removable kinds (logs, dumps, game files)."""
    out = []
    try:
        with zipfile.ZipFile(path) as z:
            for i in z.infolist():
                if i.is_dir() or i.file_size > MAX_INNER_BYTES:
                    continue
                if _logic.file_kind(i.filename) in _logic.REMOVABLE_KINDS:
                    out.append((i.filename, z.read(i)))
    except (zipfile.BadZipFile, OSError, NotImplementedError, RuntimeError):
        return out
    return out


def free_disk(path: Path) -> int:
    p = Path(path)
    while not p.exists() and p != p.parent:
        p = p.parent
    return shutil.disk_usage(p).free


def enough_disk(path: Path, need_bytes: int, reserve=1_500_000_000) -> bool:
    """True when `need_bytes` fit and `reserve` stays free. Callers check before each download."""
    return free_disk(path) >= need_bytes + reserve
