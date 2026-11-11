"""
Frame-by-frame look at what FastBar puts on screen (see record.c).

    python tests\\record.py e 900 o 1200           record this mouse script, analyse
    python tests\\record.py --name back e 900 o 150 e 900 o 1200

Steps: e = glide to the bottom edge, b = jump onto the bar, o = glide out to
mid screen, q = Quick Settings, n = calendar, x = Esc, w = Win (Start),
c<x>,<y> / r<x>,<y> = glide there and left / right click, F<title> = bring that
window to the front, a number = wait that many ms. E.g. the tray's hidden icons
opened from Start over Riot Client (the chevron at 1599,1049 here):

    python tests\\record.py --name riot "FRiot Client" 800 w 900 c1599,1049 1500 x 300 x 600 o 1500

Needs numpy and Pillow, a bottom
taskbar, and nothing moving on screen behind the bar.

While the bar stands fully shown (the real bar, or FastBar's cover with the
live thumbnail at its place) every frame must look the same: a frame that
differs is the bar blinking or being drawn twice when the two swap. Prints
those frames and writes <name>-<n>.png strips of the frames around each swap
to tests\\out\\.
"""
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
EXE = os.path.join(OUT, "record.exe")
LINE = re.compile(r"([\d.]+) tb=(-?\d+) v(\d) a(\d+) cov=(\d) slide=(-?\d+)")


def build():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(HERE, "record.c")
    if os.path.exists(EXE) and os.path.getmtime(EXE) >= os.path.getmtime(src):
        return
    r = subprocess.run(["gcc", "-O2", "-Wall", src, "-o", EXE, "-lgdi32"], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)


def load(prefix):
    with open(prefix + ".txt") as f:
        w, h, _, y0, top = (int(v) for v in f.readline().split())
        frames, marks = [], []
        for line in f:
            if line.startswith("--"):
                marks.append((len(frames), line[2:].strip()))
                continue
            m = LINE.match(line)
            if m:
                frames.append(dict(t=float(m[1]), y=int(m[2]), v=int(m[3]), a=int(m[4]),
                                   cov=int(m[5]), slide=int(m[6])))
    px = np.fromfile(prefix + ".raw", dtype=np.uint8)
    n = min(len(frames), px.size // (w * h * 4))
    px = px[:n * w * h * 4].reshape(n, h, w, 4)[..., :3].astype(np.int16)
    return frames[:n], marks, px, top - y0, top


def state(f, top):
    """what should be on screen: 'real', 'cover' (thumbnail at its place),
    'slide', 'none', or '?' (real bar out of place or half visible)"""
    real = f["v"] and f["a"] == 255
    if real and f["cov"]:
        return "both"
    if real:
        return "real" if f["y"] == top else "?"
    if f["cov"]:
        return "cover" if f["slide"] == 0 else "slide"
    return "none"


def analyse(prefix, verbose=True):
    frames, marks, px, bar0, top = load(prefix)
    n = len(frames)
    st = [state(f, top) for f in frames]
    full = [i for i in range(n) if st[i] in ("real", "cover")]
    fails = []
    if not full:
        return ["the bar never stood fully shown"], st
    # reference: the bar as it stands (median over the frames of the real bar)
    reals = [i for i in full if st[i] == "real"] or full
    ref = np.median(px[reals], axis=0)
    bar = slice(bar0, None)
    d = np.abs(px[:, bar] - ref[bar]).mean(axis=(1, 2, 3))
    # a slide that just ended / is about to start may be a pixel off for a frame
    edge = set()
    for i in range(n):
        if st[i] == "slide":
            edge.update((i - 2, i - 1, i + 1, i + 2))
    noise = float(np.median(d[reals])) if reals else 0.0
    bad = [i for i in full if d[i] > noise + 1.0 and i not in edge]
    if bad:
        fails.append("bar differs from itself while fully shown in %d frames: %s" % (
            len(bad), ", ".join("#%d %.0f ms (%s, %.1f)" % (i, frames[i]["t"], st[i], d[i]) for i in bad[:12])))
    both = [i for i in range(n) if st[i] == "both"]
    odd = [i for i in range(n) if st[i] == "?"]
    if odd:
        fails.append("real bar visible out of place in %d frames (from %.0f ms)" % (len(odd), frames[odd[0]]["t"]))
    # the slide moves every frame
    runs, cur = [], []
    for i in range(n):
        if st[i] == "slide":
            cur.append(i)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    for r in runs:
        steps = [frames[b]["slide"] - frames[a]["slide"] for a, b in zip(r, r[1:])]
        stalls = sum(1 for s in steps if s == 0)
        if stalls > 1:
            fails.append("slide at %.0f ms stalled in %d of %d frames" % (frames[r[0]]["t"], stalls, len(r)))
    if verbose:
        dt = np.diff([f["t"] for f in frames])
        print("%d frames, %.1f ms each (max %.1f); noise %.2f; both drawn in %d frames; slides: %s" % (
            n, float(np.median(dt)), float(dt.max()), noise, len(both),
            ", ".join("%.0f ms/%d fr" % (frames[r[-1]]["t"] - frames[r[0]]["t"], len(r)) for r in runs) or "-"))
        # strips around every change of state
        k = 0
        for i in range(1, n):
            if st[i] != st[i - 1] and {st[i], st[i - 1]} & {"real", "cover", "both"}:
                a, b = max(0, i - 4), min(n, i + 6)
                strip = np.concatenate([px[j] for j in range(a, b)], axis=0).astype(np.uint8)[..., ::-1]
                name = "%s-%d.png" % (prefix, k)
                Image.fromarray(strip).save(name)
                print("  #%d %.0f ms: %s -> %s   frames %d..%d in %s   diff %s" % (
                    i, frames[i]["t"], st[i - 1], st[i], a, b - 1, os.path.basename(name),
                    " ".join("%.1f" % d[j] for j in range(a, b))))
                k += 1
    return fails, st


def main():
    args = sys.argv[1:]
    name = "rec"
    if args[:1] == ["--name"]:
        name = args[1]
        args = args[2:]
    build()
    prefix = os.path.join(OUT, name)
    if args:
        if subprocess.run([EXE, prefix] + args).returncode:
            sys.exit("record.exe failed")
    fails, _ = analyse(prefix)
    for x in fails:
        print("  ! " + x)
    print("PASS" if not fails else "FAIL")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
