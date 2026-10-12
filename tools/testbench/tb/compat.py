"""Import paths and optional third-party modules in one place (pyserial, Pillow, pillow-heif)."""
import events as events_mod       # noqa: E402,F401
import imagestore                 # noqa: E402,F401
import s2s2_core as CORE          # noqa: E402,F401  tools/config
import saf_tool                   # noqa: E402,F401  tools/testbench (also a command line tool)



def cubus_on():
    """True when the configuration (.s2s2.config, TUI Build options) selects the RSMS CUBUS V.3 flight board for the satellite."""
    try:
        return bool(CORE.load_config().get("cubus"))
    except Exception:
        return False


try:
    import serial                 # noqa: F401
except ImportError:               # pragma: no cover
    serial = None
try:
    from PIL import Image, ImageFile, ImageOps          # noqa: F401
    ImageFile.LOAD_TRUNCATED_IMAGES = True              # a cut-off JPEG/PNG still gives a picture instead of an error
    Image.MAX_IMAGE_PIXELS = 400_000_000                # big panoramas / scans are fine (default limit is 89 MP)
except ImportError:               # pragma: no cover
    Image = ImageFile = ImageOps = None
try:                              # iPhone / Mac photos are HEIC; optional extra (pip install pillow-heif)
    import pillow_heif
    pillow_heif.register_heif_opener()
except ImportError:               # pragma: no cover
    pass

__all__ = ["events_mod", "imagestore", "CORE", "saf_tool", "serial", "Image", "ImageFile", "ImageOps", "cubus_on"]
