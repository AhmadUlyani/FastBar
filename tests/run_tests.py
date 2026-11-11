"""
On-screen tests for FastBar: drives the real mouse/keyboard against the
running FastBar and checks what actually showed up on screen, frame by frame.

    python tests\\run_tests.py              all scenarios
    python tests\\run_tests.py qs cal       only these
    python tests\\run_tests.py --rebuild    rebuild + restart FastBar first

Needs gcc (MinGW) on PATH, a bottom taskbar, and hands off the mouse while
it runs (about 10 s per scenario). Results and recordings go to tests\\out\\
(<scenario>.log per frame, <scenario>.bmp = kymograph: time left to right,
the bottom of the screen top to bottom).
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from shell_tests import SHELL_TESTS  # noqa: E402

ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, "out")
PROBE = os.path.join(OUT, "probe.exe")

SCENARIOS = [
    ("app", None, "leave the bar, no flyout"),
    ("qs", None, "Quick Settings open, leave the bar"),
    ("cal", None, "calendar open, leave the bar"),
    ("qs-back", None, "Quick Settings open, leave and come back after 150 ms"),
    ("qs-rapid", 200, "Quick Settings open, in/out every 200 ms"),
    ("qs-rapid", 120, "Quick Settings open, in/out every 120 ms"),
    ("cal-rapid", 350, "calendar open, in/out every 350 ms"),
    ("edge", -1, "touch the edge and leave, 20x at random times (ShowDelay)"),
    ("edge-jitter", 900, "rest on the edge, cursor slips 3 px up and back (ShowDelay)"),
    ("edge-sweep", 900, "mouse keeps moving left/right along the edge (ShowDelay)"),
    ("edge-wobble", 900, "same, drifting up to 15 px up and down (ShowDelay)"),
    ("edge-pull", 900, "real mouse: pull down past the edge, keep pushing"),
    ("edge-pull-sweep", 900, "real mouse: pull down, then left/right pressing down"),
    ("tray", None, "tray's hidden icons stay open with the cursor moving along the bar"),
    ("fullscreen", None, "bar stays away over a full-screen browser (opens Chrome/Edge)"),
    ("win-flyout", None, "over a full-screen-like window: Win, flyout, back on the bar"),
    ("resolution", None, "resolution changes (temporarily): the bar keeps fitting the screen"),
    ("menus", None, "a menu opened from the bar keeps it up while open, not after"),
    ("keyboard", None, "Win+T / Win+B bring the bar up and keep it live"),
]

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32


# ---------------------------------------------------------------- helpers
def ini_int(key, default):
    try:
        with open(os.path.join(ROOT, "FastBar.ini"), encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = re.match(r"\s*%s\s*=\s*(-?\d+)" % key, line)
                if m:
                    return int(m.group(1))
    except OSError:
        pass
    return default


def running_fastbar():
    """(pid, start time as FILETIME int) of the running FastBar, or None"""
    h = user32.FindWindowW("FastBarMain", "FastBar")
    if not h:
        return None
    pid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    hp = kernel32.OpenProcess(0x1000, False, pid.value)  # QUERY_LIMITED_INFORMATION
    if not hp:
        return pid.value, None
    c, e, k, u = (wt.FILETIME() for _ in range(4))
    kernel32.GetProcessTimes(hp, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    kernel32.CloseHandle(hp)
    return pid.value, (c.dwHighDateTime << 32) | c.dwLowDateTime


def filetime_to_unix(ft):
    return ft / 1e7 - 11644473600


def check_fresh():
    """warn when the running FastBar is older than the source"""
    src = os.path.getmtime(os.path.join(ROOT, "fastbar.c"))
    exe = os.path.join(ROOT, "FastBar.exe")
    rf = running_fastbar()
    if not rf:
        print("FastBar is not running. Start it (or use --rebuild).")
        sys.exit(1)
    if not os.path.exists(exe) or os.path.getmtime(exe) < src:
        print("WARNING: FastBar.exe is older than fastbar.c - run rebuild.bat (or --rebuild).")
    elif rf[1] and filetime_to_unix(rf[1]) < os.path.getmtime(exe):
        print("WARNING: the running FastBar started before FastBar.exe was built - run rebuild.bat.")


def build_probe():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(HERE, "probe.c")
    if os.path.exists(PROBE) and os.path.getmtime(PROBE) >= os.path.getmtime(src):
        return
    r = subprocess.run(["gcc", "-O2", "-Wall", src, "-o", PROBE, "-lgdi32"], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit("could not build probe.exe")


def read_bmp(path):
    """32-bit top-down BMP written by probe -> (w, h, rows of (r,g,b))"""
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    h = abs(h)
    px = [[None] * w for _ in range(h)]
    for y in range(h):
        base = off + y * w * 4
        for x in range(w):
            b, g, r = data[base + x * 4: base + x * 4 + 3]
            px[y][x] = (r, g, b)
    return w, h, px


LINE = re.compile(r"\s*([\d.]+) tb=(-?\d+),(-?\d+) v(\d) a(\d+)(.*)")
OV = re.compile(r"ov=(-?\d+),(-?\d+) v1 a(\d+)")
OV_OPAQUE = re.compile(r"ov=(-?\d+),(-?\d+) v1 a(\d+)(?! s)")   # see-through ones end in " s"


def read_log(path):
    frames, header = [], ""
    with open(path) as f:
        for line in f:
            if line.startswith("scenario"):
                header = line
                continue
            m = LINE.match(line)
            if m:
                frames.append(dict(t=float(m[1]), tbx=int(m[2]), tby=int(m[3]), tbv=int(m[4]),
                                   tba=int(m[5]), ov=[(int(a), int(b), int(c)) for a, b, c in OV.findall(m[6])],
                               cover=bool(OV_OPAQUE.findall(m[6]))))
    g = re.search(r"bar (-?\d+),(-?\d+),(-?\d+),(-?\d+)", header)
    top = int(g[2]) if g else None
    return frames, top


# ---------------------------------------------------------------- checks
def analyse(name, flyout, frames, top, kymo):
    """returns (failures, notes)"""
    fails, notes = [], []
    if top is None or not frames:
        return ["no data"], notes

    # 1. Explorer's own 30 fps animation must never be what is on screen:
    #    the real bar visible, opaque, part-way (FastBar's cover is see-through
    #    and does not hide it)
    explorer = [f["t"] for f in frames if f["tbv"] and f["tba"] > 1 and f["tby"] != top and not f["cover"]]
    if explorer:
        fails.append("Explorer's own animation visible in %d frames (from %.0f ms)" % (len(explorer), explorer[0]))

    # 2. with a flyout open the real bar can sit above our overlay, so it
    #    must be invisible whenever it is out of place under an overlay
    if flyout:
        ghost = [f["t"] for f in frames if f["tbv"] and f["tba"] > 1 and f["tby"] != top and f["ov"]]
        if ghost:
            fails.append("real bar visible while moving under overlay in %d frames (from %.0f ms)" % (len(ghost), ghost[0]))

    # 3. single hides (app/qs/cal): our slide runs every frame, ends hidden
    if "-" not in name:
        ys = [(f["t"], max(y for _, y, _ in f["ov"])) for f in frames if f["ov"]]
        moving = [(t, y) for t, y in ys if y != top]
        if not moving:
            fails.append("no FastBar slide seen")
        else:
            steps = [b[1] - a[1] for a, b in zip(moving, moving[1:])]
            stalls = sum(1 for s in steps if s <= 0)
            dt = [b["t"] - a["t"] for a, b in zip(frames, frames[1:])]
            frame_ms = sorted(dt)[len(dt) // 2] if dt else 0
            notes.append("slide starts %.0f ms after leaving, %d frames, %.0f ms, frame %.1f ms"
                         % (moving[0][0], len(moving), moving[-1][0] - moving[0][0], frame_ms))
            if stalls > 1:
                fails.append("slide stalled in %d frames" % stalls)
        last = frames[-1]
        if last["ov"] or (last["tbv"] and last["tba"] > 1):
            fails.append("bar not hidden at the end")
    if name == "qs-back":
        last = frames[-1]
        if not (last["tbv"] and last["tba"] == 255 and last["tby"] == top and not last["ov"]):
            fails.append("bar not live and shown after coming back")
    if name.endswith("-rapid"):
        last = frames[-1]
        if last["ov"] or (last["tbv"] and last["tba"] > 1):
            fails.append("bar not hidden at the end")
        notes.append("max overlays at once: %d" % max(len(f["ov"]) for f in frames))
    return fails, notes


def analyse_edge(path, slack=100):
    """edge scenario: the bar must come up when the cursor rests on the edge,
    go away after leaving, and the real bar must never show out of place
    (Explorer's own auto-hide slide next to ours = a second taskbar)"""
    fails, notes = [], []
    top, sh, phases = None, None, []
    with open(path) as f:
        for line in f:
            if line.startswith("scenario"):
                top = int(re.search(r"bar (-?\d+),(-?\d+)", line)[2])
                sh = int(re.search(r"screen \d+x(\d+)", line)[1])
            elif line.startswith("--"):
                k = line.split()
                phases.append((k[0][2:], int(k[1]), int(k[2]) if len(k) > 2 else 900, []))
            else:
                m = LINE.match(line)
                c = re.search(r"cur=(-?\d+),(-?\d+)", line)
                if m and phases:
                    phases[-1][3].append(dict(t=float(m[1]), y=int(m[3]), v=int(m[4]), a=int(m[5]),
                                              ov=bool(OV.findall(m[6])),
                                              cover=bool(OV_OPAQUE.findall(m[6])),
                                              cy=int(c[2]) if c else None))
    if top is None or not phases:
        return ["no data"], notes
    stuck = hidden = shift = 0
    delay = ini_int("ShowDelay", 0)
    starts = []
    for kind, cycle, ms, fr in phases:
        if not fr:
            continue
        # from a fully hidden bar: time from the cursor reaching the bottom row
        # (as recorded per frame) to the real bar being placed (= slide starts)
        if kind == "in" and ms > 700 and not fr[0]["v"] and not fr[0]["ov"]:
            touch = next((x["t"] for x in fr if x["cy"] is None or x["cy"] >= sh - 1), None)
            s = next((x["t"] - touch for x in fr if x["v"]), None) if touch is not None else None
            if s is not None:
                starts.append(max(s, 0))
        last = fr[-1]
        live = last["v"] and last["a"] == 255 and last["y"] == top and not last["ov"]
        if kind == "in" and ms > 700 and not live:
            stuck += 1
        if kind == "out" and ms > 700 and (last["ov"] or (last["v"] and last["a"] > 1)):
            hidden += 1
        # a few pixels for a frame or two is the time it takes to notice
        # Explorer moving the bar; more is a visible second taskbar
        if any(x["v"] and x["a"] > 1 and x["y"] - top > 12 and not x["cover"] for x in fr):
            shift += 1
    if stuck:
        fails.append("bar did not come up in %d edge touches" % stuck)
    if hidden:
        fails.append("bar not hidden after leaving in %d cases" % hidden)
    if shift:
        fails.append("real bar visible out of place in %d phases" % shift)
    # the slide must start ShowDelay after the touch, every time (the timer ticks
    # in 16 ms steps). A cursor wobbling up and down may first have to come
    # back near the edge.
    late = [s for s in starts if s > delay + slack]
    if late:
        fails.append("slide started late (%s ms, ShowDelay=%d)" % (", ".join("%.0f" % s for s in late), delay))
    notes.append("ShowDelay=%d, %d edge touches, slide starts after %s ms" % (
        delay, sum(1 for p in phases if p[0] == "in"),
        "%.0f-%.0f" % (min(starts), max(starts)) if starts else "-"))
    return fails, notes


def save_png(bmp, png):
    try:
        from PIL import Image
        im = Image.open(bmp)
        im.resize((im.size[0] * 2, im.size[1] * 3), Image.NEAREST).save(png)
    except Exception:
        pass


# ---------------------------------------------------------------- main
def main():
    args = [a for a in sys.argv[1:]]
    if "--rebuild" in args:
        args.remove("--rebuild")
        if subprocess.run(["cmd", "/c", os.path.join(ROOT, "rebuild.bat")]).returncode:
            sys.exit("rebuild failed")
        time.sleep(2)
    check_fresh()
    build_probe()
    todo = [s for s in SCENARIOS if not args or s[0] in args]
    if not todo:
        sys.exit("unknown scenario; choose from: " + ", ".join(sorted({s[0] for s in SCENARIOS})))
    print("Hands off the mouse and keyboard: %d scenario(s)...\n" % len(todo))
    failed = 0
    for name, period, desc in todo:
        tag = name + ("-%d" % period if period and period > 0 else "")
        if name in SHELL_TESTS:
            fails, notes = SHELL_TESTS[name]()
            skipped = not fails and any(n.startswith("skipped") for n in notes)
            print("%-14s %s  - %s" % (tag, "SKIP" if skipped else "PASS" if not fails else "FAIL", desc))
            for n in notes:
                print("               " + n)
            for x in fails:
                print("               ! " + x)
            failed += bool(fails)
            continue
        prefix = os.path.join(OUT, tag)
        cmd = [PROBE, name, prefix] + ([str(period)] if period else [])
        r = subprocess.run(cmd)
        time.sleep(1)
        if r.returncode:
            print("%-14s ERROR (probe exit %d)" % (tag, r.returncode))
            failed += 1
            continue
        if name.startswith("edge"):
            fails, notes = analyse_edge(prefix + ".log", 160 if name == "edge-wobble" else 100)
        else:
            frames, top = read_log(prefix + ".log")
            kymo = read_bmp(prefix + ".bmp")
            fails, notes = analyse(name, name.startswith(("qs", "cal")), frames, top, kymo)
            save_png(prefix + ".bmp", prefix + ".png")
        print("%-14s %s  - %s" % (tag, "PASS" if not fails else "FAIL", desc))
        for n in notes:
            print("               " + n)
        for x in fails:
            print("               ! " + x)
        failed += bool(fails)
    print("\n%d/%d passed. Recordings: %s" % (len(todo) - failed, len(todo), OUT))
    print("HideDelay=%d HideAnimationMs=%d (FastBar.ini)" % (ini_int("HideDelay", 300), ini_int("HideAnimationMs", 180)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
