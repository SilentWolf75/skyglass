#!/usr/bin/env python3
"""Capture screenshots from a running SkyGlass and save them as PNGs.

The firmware serves its live framebuffer at /shot.bmp and switches screens via
/view?i=N, so this walks the views and writes one PNG per screen. Photographs of a
glossy round AMOLED never look as good as the real pixels.

    python tools/grab_screens.py [host] [outdir]
    python tools/grab_screens.py skyglass.local docs/img/screens

Corners are made transparent to match the physical round panel, which is what makes
these look right dropped into a README. Pure stdlib - no Pillow, no ffmpeg.
"""
import os
import struct
import sys
import time
import urllib.request
import zlib

VIEWS = [
    (0, None, "radar",    "Radar scope"),
    (1, None, "list",     "Contact list"),
    (2, None, "stats",    "Stats"),
    (3, 0,    "wx-radar", "Precipitation radar"),
    (3, 1,    "wx-cloud", "Satellite clouds"),
    (3, 2,    "forecast", "3-day forecast"),
    (4, None, "tracked",  "Tracked flight"),
    (5, None, "clock",    "Clock"),
    (6, None, "about",    "About"),
    (7, None, "settings", "Settings"),
]

# The detail card needs a contact selected and then a pause: tapping one kicks off four
# lookups (route, photo, airline logo, registration) that land over several seconds.
DETAIL_SETTLE_S = 11


def get(url, timeout=60):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def read_bmp(data):
    """Minimal reader for the 24-bit bottom-up BMP the firmware emits."""
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"expected 24-bit, got {bpp}")
    row_bytes = w * 3
    pad = (4 - (row_bytes % 4)) % 4
    rows = []
    for y in range(h):
        start = off + y * (row_bytes + pad)
        rows.append(data[start:start + row_bytes])
    rows.reverse()                      # BMP stores bottom-up
    return w, h, rows


def write_png(path, w, h, rows, round_mask=True):
    """RGBA PNG. Corners outside the circle get alpha 0 so the round panel reads right."""
    cx = cy = (w - 1) / 2.0
    r2 = (min(w, h) / 2.0) ** 2
    raw = bytearray()
    for y in range(h):
        raw.append(0)                   # filter: none
        src = rows[y]
        for x in range(w):
            b, g, r = src[x * 3], src[x * 3 + 1], src[x * 3 + 2]
            a = 255
            if round_mask and ((x - cx) ** 2 + (y - cy) ** 2) > r2:
                a = 0
            raw += bytes((r, g, b, a))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "skyglass.local"
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join("docs", "img", "screens")
    os.makedirs(outdir, exist_ok=True)

    for view, wx, name, label in VIEWS:
        url = f"http://{host}/view?i={view}"
        if wx is not None:
            url += f"&wx={wx}"
        try:
            get(url, timeout=15)
        except Exception as exc:
            print(f"  {name}: could not switch view ({exc})")
            continue
        time.sleep(1.6)                 # let the tile settle and any animation land
        try:
            bmp = get(f"http://{host}/shot.bmp")
            w, h, rows = read_bmp(bmp)
            path = os.path.join(outdir, f"{name}.png")
            n = write_png(path, w, h, rows)
            print(f"  {label:22} -> {path}  ({w}x{h}, {n/1024:.0f} KB)")
        except Exception as exc:
            print(f"  {name}: capture failed ({exc})")

    # Detail card: select the nearest contact and wait for its lookups to arrive.
    try:
        get(f"http://{host}/view?i=0&sel=0", timeout=15)
        print(f"  {'Detail card':22} (waiting {DETAIL_SETTLE_S}s for lookups)")
        time.sleep(DETAIL_SETTLE_S)
        bmp = get(f"http://{host}/shot.bmp")
        w, h, rows = read_bmp(bmp)
        path = os.path.join(outdir, "detail.png")
        n = write_png(path, w, h, rows)
        print(f"  {'Detail card':22} -> {path}  ({w}x{h}, {n/1024:.0f} KB)")
    except Exception as exc:
        print(f"  detail: capture failed ({exc})")

    try:
        get(f"http://{host}/view?i=0&sel=-1", timeout=15)   # clear selection, back to radar
    except Exception:
        pass


if __name__ == "__main__":
    main()
