#!/usr/bin/env python3
#
# make_icon.py — build assets/machpsdr.ico from assets/machpsdr_icon.png.
#
# The Windows .exe carries its icon as a resource (src/core/machpsdr.rc), and a
# resource needs a real multi-size .ico: Explorer, the taskbar, Alt-Tab and the
# properties dialog each ask for a different size, and a single-size icon is
# rescaled by the shell with visibly worse results than a pre-rendered one.
#
# The .ico is COMMITTED, so this script is not part of the build — it exists so
# the binary asset is reproducible rather than a mystery blob.  Nothing about
# the build depends on it and no image library is added to the build:
#
#     tools/make_icon.py [source.png] [out.ico]
#
# Vista and later read PNG-compressed images straight out of an .ico, so the
# container is written around resized PNGs and no BMP/DIB encoder (which would
# need an image library) is required here.  The resizing itself is delegated to
# whatever the box has: `sips` (macOS, always present), ImageMagick, or PIL.

import os
import shutil
import struct
import subprocess
import sys
import tempfile

# 256 is what the shell shows at "Extra large icons" and in the Vista+ preview;
# 16/24/32/48 are the sizes Explorer, the taskbar and Alt-Tab actually pick.
SIZES = (16, 24, 32, 48, 64, 128, 256)


def resize(src, dst, size):
    """Write a `size`x`size` PNG of `src` to `dst`, using whatever is installed."""
    if shutil.which("sips"):                       # macOS, no install needed
        subprocess.run(["sips", "-z", str(size), str(size), src, "--out", dst],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        return
    for im in ("magick", "convert"):               # ImageMagick
        if shutil.which(im):
            subprocess.run([im, src, "-resize", "%dx%d" % (size, size), dst],
                           check=True)
            return
    try:                                           # last resort: PIL
        from PIL import Image
    except ImportError:
        sys.exit("error: need sips, ImageMagick or python3 PIL to resize images")
    img = Image.open(src).convert("RGBA")
    img.resize((size, size), Image.LANCZOS).save(dst)


def main(argv):
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = argv[1] if len(argv) > 1 else os.path.join(here, "assets", "machpsdr_icon.png")
    out = argv[2] if len(argv) > 2 else os.path.join(here, "assets", "machpsdr.ico")

    if not os.path.exists(src):
        sys.exit("error: no such file: %s" % src)

    images = []
    with tempfile.TemporaryDirectory() as tmp:
        for size in SIZES:
            png = os.path.join(tmp, "icon_%d.png" % size)
            resize(src, png, size)
            with open(png, "rb") as f:
                data = f.read()
            if not data.startswith(b"\x89PNG"):
                sys.exit("error: resizer did not produce a PNG at %d px" % size)
            images.append((size, data))

    # ICONDIR: reserved, type 1 (icon), image count.  Then one 16-byte
    # ICONDIRENTRY each -- a side of 256 is stored as 0, the format's only way
    # of spelling it in a byte.
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries, blobs = b"", b""
    for size, data in images:
        side = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32,
                               len(data), offset)
        blobs += data
        offset += len(data)

    with open(out, "wb") as f:
        f.write(header + entries + blobs)
    print("wrote %s (%d images, %d bytes)" % (out, len(images),
                                              os.path.getsize(out)))


if __name__ == "__main__":
    main(sys.argv)
