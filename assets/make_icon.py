"""Generates assets/FastBar.ico (and a PNG preview). Run: python assets/make_icon.py

Flat, two-color logo: a blue rounded square with a white taskbar near the
bottom and a white double chevron above it (the bar popping up, fast).
"""
import os
from PIL import Image, ImageDraw

BLUE = (31, 111, 235, 255)
WHITE = (255, 255, 255, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
HERE = os.path.dirname(os.path.abspath(__file__))


def draw(px):
    ss = 8                       # supersample, then downscale (smooth edges)
    S = px * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = S / 100.0
    pad = 2 if px >= 32 else 0
    d.rounded_rectangle([pad * u, pad * u, S - 1 - pad * u, S - 1 - pad * u],
                        radius=22 * u, fill=BLUE)
    # taskbar
    d.rounded_rectangle([18 * u, 68 * u, 82 * u, 81 * u], radius=4 * u, fill=WHITE)
    # double chevron: the bar shooting up
    w = 10 * u
    for top in (18, 37):
        apex, left, right = (50 * u, top * u), (31 * u, (top + 17) * u), (69 * u, (top + 17) * u)
        d.line([left, apex, right], fill=WHITE, width=round(w), joint="curve")
        for x, y in (left, right):
            d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=WHITE)
    return im.resize((px, px), Image.LANCZOS)


if __name__ == "__main__":
    imgs = [draw(s) for s in SIZES]
    imgs[-1].save(os.path.join(HERE, "FastBar.ico"), sizes=[(s, s) for s in SIZES],
                  append_images=imgs[:-1])
    imgs[-1].save(os.path.join(HERE, "FastBar.png"))
    # preview strip of the small sizes, scaled up 4x with nearest-neighbour
    strip = Image.new("RGBA", (sum(s for s in SIZES[:6]) * 4 + 70, 48 * 4 + 20), (32, 32, 32, 255))
    x = 10
    for im in imgs[:6]:
        strip.alpha_composite(im.resize((im.width * 4, im.height * 4), Image.NEAREST), (x, 10))
        x += im.width * 4 + 10
    strip.save(os.environ.get("PREVIEW", os.path.join(HERE, "preview.png")))
