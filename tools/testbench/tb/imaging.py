"""tb.imaging"""

import io
import math
from tb.bus import IMAGES, log
from tb.compat import Image, ImageOps
from tb.util import CHUNK, FLASH_BYTES, GS_BUF_MAX, sha256

# --------------------------------------------------------------------------------------
# image preparation
# --------------------------------------------------------------------------------------
IMG = {"name": None, "bytes": None, "orig": None, "info": "", "fit": "none"}


def kind_of(b):
    """(extension, mime type) from the magic bytes of an image file."""
    if b[:3] == b"\xff\xd8\xff":
        return "jpg", "image/jpeg"
    if b[:8] == b"\x89PNG\r\n\x1a\n":
        return "png", "image/png"
    if b[:4] == b"GIF8":
        return "gif", "image/gif"
    return "bin", "application/octet-stream"


def fit_jpeg(raw, name, max_bytes, max_w=320):
    """Re-encode as the best-looking JPEG that is <= max_bytes (largest size/quality that fits)."""
    if raw[:2] == b"\xff\xd8" and len(raw) <= max_bytes and raw.rstrip(b"\0")[-2:] == b"\xff\xd9":
        return raw, f"{name}: JPEG already fits ({len(raw)} B)"
    # (a JPEG without its end marker is a cut-off download: re-encode it below instead of sending a broken picture)
    if Image is None:
        raise RuntimeError("Pillow missing: pip install pillow")
    try:
        im = Image.open(io.BytesIO(raw))
        im.load()
    except Exception as e:
        raise RuntimeError(f"{name}: cannot read this as an image ({e}). Use JPEG, PNG, GIF, BMP, WebP"
                           f"{', HEIC' if 'pillow_heif' in globals() else ' (pip install pillow-heif for HEIC)'}") from e
    im = ImageOps.exif_transpose(im)             # phone photos: honour the EXIF rotation instead of sending them sideways
    if im.mode in ("RGBA", "LA", "P"):
        bg = Image.new("RGB", im.size, (255, 255, 255))
        im = im.convert("RGBA")
        bg.paste(im, mask=im.split()[-1])
        im = bg
    else:
        im = im.convert("RGB")
    w, h = im.size
    for scale in (1.0, 0.85, 0.7, 0.55, 0.4, 0.3, 0.2):
        tw = max(16, int(min(max_w, w) * scale))
        th = max(16, int(tw * h / w))
        r = im.resize((tw, th), Image.LANCZOS)
        for q in (90, 85, 75, 65, 55, 45, 35, 25):
            b = io.BytesIO()
            r.save(b, "JPEG", quality=q, optimize=True)
            if b.tell() <= max_bytes:
                return b.getvalue(), f"{name}: compressed to JPEG {tw}x{th} q{q} ({b.tell()} B, was {len(raw)} B)"
    raise RuntimeError("could not shrink the image under %d B" % max_bytes)


def set_image(raw, name, mode="flash"):
    """mode: none = send the file byte for byte; flash = compress to fit the 64 KB OBC flash;
    quick = small <=10 KB JPEG for a fast demo."""
    mode = {True: "quick", False: "none"}.get(mode, mode) or "flash"
    if mode == "flash":
        data, info = fit_jpeg(raw, name, FLASH_BYTES - 1024, 480)
    elif mode == "quick":
        data, info = fit_jpeg(raw, name, GS_BUF_MAX - 64, 320)
    else:
        data, info = raw, f"{name}: sent as-is, {len(raw)} B (no conversion)"
    IMG.update(name=name, bytes=data, orig=raw, info=info, fit=mode, rx=None)
    IMAGES.expect(sha256(data), len(data))              # the next completed picture is compared with this
    log(info + f", {math.ceil(len(data) / CHUNK)} chunks, sha256 {sha256(data)[:16]}…")
    return info
