#!/usr/bin/env python3
"""Build the animated GIFs used in the README from captured device screenshots.

Unlike grab_screens.py this is NOT stdlib-only -- it needs Pillow:

    pip install pillow

That is deliberate. grab_screens.py has to run anywhere because users run it against
their own device; this one is an authoring tool that only ever runs when the README
assets are regenerated, and GIF quantisation is worth a real median-cut implementation.

Why a screen tour rather than the radar sweep: /shot.bmp takes ~2.3 s per frame over
WiFi against a ~5 s sweep period, so a sweep GIF would sample about two frames per
rotation -- a stutter, not motion. Cycling whole screens is honest at that cadence and
shows more of what the firmware does.

    python tools/make_gif.py
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow:  pip install pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "docs", "img")

# (file, milliseconds). The radar and detail card carry the story, so they hold longer.
TOUR = [
    ("screens/radar.png",    2200),
    ("screens/detail.png",   2200),
    ("screens/list.png",     1500),
    ("screens/tracked.png",  1800),
    ("screens/wx-radar.png", 1400),
    ("screens/forecast.png", 1400),
    ("screens/clock.png",    1500),
    ("screens/about.png",    1600),
]

THEMES = [
    ("theme-phosphor.png", 1300),
    ("theme-orb.png",      1300),
    ("theme-amber.png",    1300),
    ("theme-military.png", 1300),
    ("theme-red.png",      1300),
    ("theme-cyan.png",     1300),
]


def to_paletted(path, size):
    """RGBA PNG -> P-mode frame with index 255 reserved as transparent.

    The screenshots have transparent corners so the round panel reads correctly on both
    light and dark README backgrounds; flattening them onto black would put a hard square
    behind every frame.
    """
    im = Image.open(path).convert("RGBA")
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    alpha = im.getchannel("A")
    # Quantise the colour content into 0..254, leaving one slot for transparency.
    p = im.convert("RGB").quantize(colors=255, method=Image.MEDIANCUT)
    pal = p.getpalette()[: 255 * 3] + [0, 0, 0]
    p.putpalette(pal)
    p.paste(255, alpha.point(lambda a: 255 if a < 128 else 0))
    return p


def build(frames, out, size):
    imgs, durs = [], []
    for name, ms in frames:
        src = os.path.join(IMG, name)
        if not os.path.exists(src):
            print(f"  skip (missing): {name}")
            continue
        imgs.append(to_paletted(src, size))
        durs.append(ms)
    if not imgs:
        sys.exit("no frames found -- run tools/grab_screens.py first")
    dst = os.path.join(IMG, out)
    imgs[0].save(dst, save_all=True, append_images=imgs[1:], duration=durs, loop=0,
                 transparency=255, disposal=2, optimize=True)
    print(f"  {out}  {len(imgs)} frames  {os.path.getsize(dst) / 1024:.0f} KB")


def main():
    print("building GIFs:")
    build(TOUR, "tour.gif", 320)
    build(THEMES, "themes.gif", 300)


if __name__ == "__main__":
    main()
