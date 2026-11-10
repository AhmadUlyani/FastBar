"""
On-screen tests with real shell windows, used by run_tests.py:

  tray        the tray's overflow window (hidden icons), opened by a real click,
              stays open while the cursor moves along the taskbar; once closed,
              the bar stays up under the cursor and hides after leaving
  fullscreen  the bar stays away over a full-screen browser, whether or not the
              browser was maximized first, also when it goes full screen with the
              bar up; it works again after leaving full screen
  resolution  the screen resolution changes for a moment (not saved): the bar
              keeps fitting the screen, hidden or up at the time
  menus       a menu opened from the bar (bar, tray icon) keeps it up while open
  keyboard    Win+T / Win+B: the bar comes up and stays live for the keyboard
  win-flyout  over a window Windows takes for full screen: Win shows the bar, a
              flyout opened from it, the cursor back on the bar: it stays on top

Each function returns (failures, notes).
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROFILE = os.path.join(HERE, "out", "fs-profile")

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()
SW, SH = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
OVERFLOW = "TopLevelWindowForOverflowXamlIsland"
VK_ESCAPE, VK_F11 = 0x1B, 0x7A


class MI(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("data", wt.DWORD), ("flags", wt.DWORD),
                ("time", wt.DWORD), ("extra", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("mi", MI)]


class KI(ctypes.Structure):
    _fields_ = [("vk", wt.WORD), ("scan", wt.WORD), ("flags", wt.DWORD), ("time", wt.DWORD),
                ("extra", ctypes.c_size_t), ("pad", ctypes.c_byte * 8)]


class KINPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("ki", KI)]


def mouse(flags, dx=0, dy=0):
    i = INPUT(0, MI(dx, dy, 0, flags, 0, 0))
    user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(i))


def move(x, y):
    mouse(0x8001, int(x * 65535 / (SW - 1)), int(y * 65535 / (SH - 1)))


def glide(x0, y0, x1, y1, ms=150):
    n = max(1, ms // 8)
    for i in range(1, n + 1):
        move(x0 + (x1 - x0) * i // n, y0 + (y1 - y0) * i // n)
        time.sleep(0.008)


def pull_down(times=14):
    """a real mouse: relative moves, far past the bottom edge"""
    for _ in range(times):
        mouse(1, 0, 60)
        time.sleep(0.004)


def click():
    mouse(2)
    time.sleep(0.03)
    mouse(4)


def key(vk):
    for fl in (0, 2):
        i = KINPUT(1, KI(vk, 0, fl, 0, 0))
        user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(i))


def cname(h):
    b = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(h, b, 128)
    return b.value


def rect(h):
    r = wt.RECT()
    user32.GetWindowRect(h, ctypes.byref(r))
    return r


def fg_class():
    return cname(user32.GetForegroundWindow())


def bar_up():
    return bool(user32.IsWindowVisible(user32.FindWindowW("Shell_TrayWnd", None)))


def overlays():
    n, h = 0, None
    while True:
        h = user32.FindWindowExW(None, h, "FastBarSnapshot", None)
        if not h:
            return n
        n += bool(user32.IsWindowVisible(h))


def overflow_open():
    found = []
    proto = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        if cname(h) == OVERFLOW and user32.IsWindowVisible(h):
            v = wt.DWORD()
            ctypes.windll.dwmapi.DwmGetWindowAttribute(wt.HWND(h), 14, ctypes.byref(v), 4)
            if not v.value:
                found.append(h)
        return True

    user32.EnumWindows(proto(cb), 0)
    return bool(found)


def away():
    move(SW // 2, SH // 2)
    time.sleep(0.9)


# ------------------------------------------------------------------- tray
def find_chevron():
    """(x, y) of the "Show hidden icons" button: the first plain tray button"""
    glide(SW // 2, SH // 2, SW // 2, SH - 1, 100)
    time.sleep(0.6)
    ps = ("Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes;"
          "$t=[System.Windows.Automation.AutomationElement]::FromHandle(%d);"
          "$c=New-Object System.Windows.Automation.PropertyCondition("
          "[System.Windows.Automation.AutomationElement]::ClassNameProperty,'SystemTray.NormalButton');"
          "$e=$t.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$c);"
          "if($e){$r=$e.Current.BoundingRectangle;'{0},{1},{2},{3}' -f $r.X,$r.Y,$r.Width,$r.Height}"
          % user32.FindWindowW("Shell_TrayWnd", None))
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    m = re.match(r"(-?[\d.]+),(-?[\d.]+),([\d.]+),([\d.]+)", out.strip())
    tb = user32.FindWindowW("Shell_TrayWnd", None)
    notify = user32.FindWindowExW(tb, None, "TrayNotifyWnd", None)
    away()
    if not m:
        # UI Automation sometimes reports an empty taskbar: the button is the
        # first thing in the tray (a wrong spot fails: the overflow does not open)
        if not notify:
            return None
        r, b = rect(notify), rect(tb)
        return r.left + 20, (b.top + b.bottom) // 2
    x, y, w, h = map(float, m.groups())
    return int(x + w / 2), int(y + h / 2)


def open_overflow(chev):
    glide(SW // 2, SH // 2, chev[0], SH - 1, 150)
    time.sleep(0.5)
    move(*chev)
    time.sleep(0.5)
    click()
    time.sleep(0.3)
    return overflow_open()


def stays_open(secs):
    """ms after which the overflow window closed, None if it stayed open"""
    t0 = time.time()
    while time.time() - t0 < secs:
        if not overflow_open():
            return (time.time() - t0) * 1000
        time.sleep(0.01)
    return None


def run_tray():
    fails, notes = [], []
    away()
    chev = find_chevron()
    if not chev:
        return [], ["skipped: no hidden-icons button on this taskbar"]
    paths = {
        "cursor stays on the button": lambda: None,
        "cursor moves along the bar": lambda: glide(chev[0], chev[1], chev[0] - 700, chev[1], 400),
    }
    for name, go in paths.items():
        if not open_overflow(chev):
            fails.append("overflow did not open (%s)" % name)
            continue
        go()
        closed = stays_open(2.5)
        if closed is not None:
            fails.append("overflow closed by itself, %s, after %.0f ms" % (name, closed))
        if not bar_up():
            fails.append("bar gone with the overflow open, %s" % name)
        key(VK_ESCAPE)
        time.sleep(0.5)
        away()

    # into the flyout, back onto the bar
    if open_overflow(chev):
        glide(chev[0], chev[1], chev[0], chev[1] - 90, 150)
        time.sleep(1.2)
        glide(chev[0], chev[1] - 90, chev[0] - 500, chev[1], 200)
        closed = stays_open(2.0)
        if closed is not None:
            fails.append("overflow closed after coming back from it, after %.0f ms" % closed)
        key(VK_ESCAPE)
        time.sleep(0.5)
        away()

    # closed with the cursor on the bar: the bar stays, also after the sync,
    # and goes away after leaving
    if open_overflow(chev):
        key(VK_ESCAPE)
        time.sleep(1.2)
        if not bar_up() or overlays():
            fails.append("bar not live under the cursor after closing the overflow")
        away()
        time.sleep(0.5)
        if bar_up() or overlays():
            fails.append("bar not hidden after closing the overflow and leaving")
    return fails, notes


# ------------------------------------------------------------- fullscreen
def find_chrome():
    for p in (r"%ProgramFiles%\Google\Chrome\Application\chrome.exe",
              r"%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe",
              r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe",
              r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"):
        p = os.path.expandvars(p)
        if os.path.exists(p):
            return p
    return None


def close_browser():
    """only the instance of this test (its own profile)"""
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*fs-profile*' -and "
                    "$_.Name -match 'chrome|msedge' } | "
                    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"],
                   capture_output=True)
    time.sleep(1.5)


def open_browser(exe, maximized):
    page = "file:///" + os.path.join(HERE, "fullscreen.html").replace("\\", "/")
    args = [exe, "--user-data-dir=" + PROFILE, "--no-first-run", "--no-default-browser-check", "--new-window"]
    args += ["--start-maximized"] if maximized else ["--window-position=200,100", "--window-size=1100,700"]
    subprocess.Popen(args + [page])
    time.sleep(4)


def samples(secs=1.0):
    """frames in which the bar or an overlay of FastBar was up"""
    n, end = 0, time.time() + secs
    while time.time() < end:
        n += bar_up() or overlays() > 0
        time.sleep(0.016)
    return n


def run_fullscreen_case(exe, maximized):
    tag = "maximized first" if maximized else "not maximized first"
    fails = []
    open_browser(exe, maximized)
    try:
        h = user32.GetForegroundWindow()
        move(SW // 2, SH // 2)
        time.sleep(0.3)
        click()
        time.sleep(2)
        r = rect(h)
        if not (r.left == 0 and r.top == 0 and r.right == SW and r.bottom == SH):
            return ["browser did not go full screen (%s)" % tag]
        for how in ("absolute", "relative"):
            move(SW // 3, SH // 2)
            time.sleep(0.8)
            if how == "absolute":
                glide(SW // 3, SH // 2, SW // 3, SH - 1, 100)
            else:
                pull_down()
            n = samples()
            if n:
                fails.append("bar came up over the full-screen window, %s, %s mouse (%d samples)" % (tag, how, n))
        # leave full screen: the bar works again (a window that was maximized
        # covers the screen as well, and must not count as full screen)
        away()
        key(VK_ESCAPE)
        time.sleep(1.5)
        glide(SW // 3, SH // 2, SW // 3, SH - 1, 100)
        time.sleep(0.8)
        if not bar_up():
            fails.append("bar did not come up after leaving full screen (%s)" % tag)
        away()
        # full screen with the bar up: it goes at once, without a slide
        glide(SW // 3, SH // 2, SW // 3, SH - 1, 100)
        time.sleep(0.6)
        move(SW // 3, SH - 20)
        time.sleep(0.4)
        key(VK_F11)
        time.sleep(1.5)
        mouse(1, 2, 0)                      # any mouse event: FastBar looks again
        slid, end = 0, time.time() + 0.8
        while time.time() < end:
            slid += overlays() > 0
            time.sleep(0.008)
        if slid:
            fails.append("a picture was shown while hiding for full screen (%s, %d samples)" % (tag, slid))
        if bar_up():
            fails.append("bar still up after going full screen (%s)" % tag)
        away()
        key(VK_F11)
        time.sleep(1)
    finally:
        close_browser()
    return fails


def bar_on_top():
    """the real bar is what is at the middle of the bar's area (click-through
    pictures of FastBar do not count), not a window that covers it"""
    tb = user32.FindWindowW("Shell_TrayWnd", None)
    r = rect(tb)
    pt = wt.POINT((r.left + r.right) // 2, (r.top + r.bottom) // 2)
    user32.GetAncestor.restype = wt.HWND
    user32.GetAncestor.argtypes = (wt.HWND, wt.UINT)
    user32.WindowFromPoint.restype = wt.HWND
    return user32.GetAncestor(user32.WindowFromPoint(pt), 2) == tb


def run_flyout_over(chev, front, tag):
    """Win (shows the bar), open a flyout by a click, hover along the bar"""
    fails = []
    x, y = chev
    away()
    if not front():
        return ["%s: window not in front" % tag]
    key(0x5B)
    time.sleep(1.2)
    glide(SW // 2, SH // 2, x, y, 250)
    time.sleep(0.5)
    click()
    time.sleep(0.6)
    glide(x, y, x - 700, y, 400)
    bad, n, end = 0, 0, time.time() + 1.5
    time.sleep(0.2)                         # the go-live of the bar
    while time.time() < end:
        n += 1
        bad += not (bar_up() and bar_on_top())
        time.sleep(0.016)
    if bad:
        fails.append("%s: bar gone under the cursor in %d of %d samples" % (tag, bad, n))
    if tag == "tray" and not overflow_open():
        fails.append("%s: overflow closed by itself" % tag)
    key(VK_ESCAPE)
    time.sleep(0.6)
    if fg_class() == "Windows.UI.Core.CoreWindow":
        key(VK_ESCAPE)                      # Windows: Start comes back when the calendar closes
        time.sleep(0.6)
    away()
    time.sleep(0.5)
    if bar_up() or overlays():
        fails.append("%s: bar not hidden afterwards" % tag)
    return fails


def run_win_flyout():
    """the same over a window Windows takes for full screen (bigwindow.py is
    shaped like Riot Client's): Windows puts the bar below it once Start closes"""
    chev = (None, None)
    away()
    chev = find_chevron()
    if not chev:
        return [], ["skipped: no hidden-icons button on this taskbar"]
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "bigwindow.py"), "40"])
    time.sleep(1.5)
    big = None

    def front():
        nonlocal big
        move(SW // 2, SH // 2)
        time.sleep(0.2)
        click()
        time.sleep(0.6)
        big = user32.GetForegroundWindow()
        return cname(big) == "FastBarTestBig"

    fails = []
    try:
        fails += run_flyout_over(chev, front, "tray")
        # the clock: its calendar, right of the hidden icons
        fails += run_flyout_over((SW - 120, chev[1]), front, "calendar")
    finally:
        p.kill()
        time.sleep(0.5)
    return fails, []


def run_fullscreen():
    exe = find_chrome()
    if not exe:
        return [], ["skipped: no Chrome or Edge found"]
    fails = []
    away()
    close_browser()
    try:
        for maximized in (True, False):
            fails += run_fullscreen_case(exe, maximized)
    finally:
        close_browser()
        shutil.rmtree(PROFILE, ignore_errors=True)
    return fails, []


SHELL_TESTS = {"tray": run_tray, "fullscreen": run_fullscreen, "win-flyout": run_win_flyout}


# ------------------------------------------------------------- resolution
class DEVMODE(ctypes.Structure):
    _fields_ = [("dmDeviceName", wt.WCHAR * 32), ("dmSpecVersion", wt.WORD), ("dmDriverVersion", wt.WORD),
                ("dmSize", wt.WORD), ("dmDriverExtra", wt.WORD), ("dmFields", wt.DWORD),
                ("dmPositionX", wt.LONG), ("dmPositionY", wt.LONG), ("dmDisplayOrientation", wt.DWORD),
                ("dmDisplayFixedOutput", wt.DWORD), ("dmColor", ctypes.c_short), ("dmDuplex", ctypes.c_short),
                ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short), ("dmCollate", ctypes.c_short),
                ("dmFormName", wt.WCHAR * 32), ("dmLogPixels", wt.WORD), ("dmBitsPerPel", wt.DWORD),
                ("dmPelsWidth", wt.DWORD), ("dmPelsHeight", wt.DWORD), ("dmDisplayFlags", wt.DWORD),
                ("dmDisplayFrequency", wt.DWORD), ("dmICMMethod", wt.DWORD), ("dmICMIntent", wt.DWORD),
                ("dmMediaType", wt.DWORD), ("dmDitherType", wt.DWORD), ("dmReserved1", wt.DWORD),
                ("dmReserved2", wt.DWORD), ("dmPanningWidth", wt.DWORD), ("dmPanningHeight", wt.DWORD)]


def display_modes():
    out, i = set(), 0
    dm = DEVMODE()
    dm.dmSize = ctypes.sizeof(dm)
    while user32.EnumDisplaySettingsW(None, i, ctypes.byref(dm)):
        out.add((dm.dmPelsWidth, dm.dmPelsHeight))
        i += 1
    return out


def set_resolution(w, h):
    """temporary (CDS_FULLSCREEN): not saved, undone when this process exits"""
    dm = DEVMODE()
    dm.dmSize = ctypes.sizeof(dm)
    dm.dmPelsWidth, dm.dmPelsHeight = w, h
    dm.dmFields = 0x80000 | 0x100000
    return user32.ChangeDisplaySettingsExW(None, ctypes.byref(dm), None, 4, None) == 0


def restore_resolution():
    user32.ChangeDisplaySettingsExW(None, None, None, 0, None)


def screen_size():
    global SW, SH
    SW, SH = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    return SW, SH


def bar_fits(tag):
    """the shown bar sits on the bottom edge, as wide as the screen"""
    tb = user32.FindWindowW("Shell_TrayWnd", None)
    r = rect(tb)
    if not user32.IsWindowVisible(tb):
        return ["%s: bar not up" % tag]
    if r.bottom != SH or r.left != 0 or r.right != SW:
        return ["%s: bar at %d,%d,%d,%d on a %dx%d screen" % (tag, r.left, r.top, r.right, r.bottom, SW, SH)]
    return []


def run_resolution():
    """Resolution (and with it the scaling) changes: the bar must keep fitting
    the screen, whether it is hidden or up at the time of the change."""
    w0, h0 = screen_size()
    small = next((m for m in [(1440, 1080), (1280, 1024), (1280, 960), (1600, 900)]
                  if m in display_modes() and m != (w0, h0)), None)
    if not small:
        return [], ["skipped: no other display mode to switch to"]
    fails = []
    away()
    try:
        for step in (small, None, small, None):
            # changed while hidden: hover afterwards
            set_resolution(*step) if step else restore_resolution()
            time.sleep(2.5)
            screen_size()
            tag = "%dx%d" % (step if step else (w0, h0))
            away()
            glide(SW // 2, SH // 2, SW // 3, SH - 1, 100)
            time.sleep(0.4)
            move(SW // 3, SH - 10)
            time.sleep(0.8)
            fails += bar_fits(tag + ", changed while hidden")
            away()
            time.sleep(0.6)
            if bar_up() or overlays():
                fails.append(tag + ": bar not hidden after leaving")
        # changed with the bar up: the cursor stays on the bottom edge
        for step in (small, None):
            screen_size()
            glide(SW // 2, SH // 2, SW // 3, SH - 1, 100)
            time.sleep(0.8)
            set_resolution(*step) if step else restore_resolution()
            time.sleep(2.5)
            screen_size()
            move(SW // 3, SH - 1)
            time.sleep(0.8)
            fails += bar_fits("%dx%d, changed with the bar up" % (step if step else (w0, h0)))
            away()
            time.sleep(0.6)
    finally:
        restore_resolution()
        time.sleep(2)
        screen_size()
    return fails, ["switched between %dx%d and %dx%d" % (w0, h0, small[0], small[1])]


SHELL_TESTS["resolution"] = run_resolution


# ------------------------------------------------------------ menus, keys
def bar_seen():
    """the bar is on screen: the live bar on top, or FastBar's picture of it"""
    return (bar_up() and bar_on_top()) or overlays() > 0


def menus_open():
    found = []
    proto = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        c = cname(h)
        if c in ("#32768", "Xaml_WindowedPopupClass") and user32.IsWindowVisible(h):
            r = rect(h)
            if r.right - r.left > 20 and r.bottom - r.top > (64 if c != "#32768" else 8):
                found.append(c)
        return True
    user32.EnumWindows(proto(cb), 0)
    return found


def rclick():
    mouse(8)
    time.sleep(0.03)
    mouse(16)


def run_menus():
    """a menu opened from the bar (right-click on the bar, on a tray icon) keeps
    the bar up while the cursor is away; closing it lets the bar go"""
    fails = []
    away()
    chev = find_chevron()
    spots = [("bar menu", SW * 15 // 100)]
    if chev:
        spots.append(("tray icon menu", chev[0] + 40))
    y = SH - 30
    for tag, x in spots:
        away()
        glide(SW // 2, SH // 2, x, SH - 1, 120)
        time.sleep(0.5)
        move(x, y)
        time.sleep(0.4)
        rclick()
        time.sleep(0.8)
        if not menus_open():
            fails.append("%s: no menu opened" % tag)
            key(VK_ESCAPE)
            continue
        glide(x, y, SW // 6, SH // 4, 200)
        time.sleep(1.2)
        if not menus_open():
            fails.append("%s: menu closed by itself" % tag)
        elif not bar_seen():
            fails.append("%s: bar gone while its menu is open" % tag)
        key(VK_ESCAPE)                      # the cursor stays where it is
        time.sleep(1.0)
        if bar_seen():
            fails.append("%s: bar still up after the menu closed" % tag)
    away()
    return fails, []


def combo(*vks):
    for vk in vks:
        i = KINPUT(1, KI(vk, 0, 0, 0, 0))
        user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(i))
    for vk in reversed(vks):
        i = KINPUT(1, KI(vk, 0, 2, 0, 0))
        user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(i))


def run_keyboard():
    """Win+T / Win+B put the keyboard focus on the bar: it comes up, stays live
    while arrows move the focus, and goes once another window is clicked"""
    fails = []
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "bigwindow.py"), "40"])
    time.sleep(1.5)
    try:
        for tag, k in (("Win+T", 0x54), ("Win+B", 0x42)):
            away()
            click()                             # the test window in front
            time.sleep(0.6)
            if cname(user32.GetForegroundWindow()) != "FastBarTestBig":
                fails.append("%s: test window not in front" % tag)
                continue
            combo(0x5B, k)
            time.sleep(0.7)
            bad = 0
            for _ in range(5):
                key(0x27)                       # Right: the focus moves along the bar
                time.sleep(0.25)
                bad += not (bar_up() and bar_on_top()) or overlays() > 0
            if bad:
                fails.append("%s: bar missing or frozen in %d of 5 samples" % (tag, bad))
            click()                             # focus to the test window
            time.sleep(1.0)
            if bar_seen():
                fails.append("%s: bar still up after clicking another window" % tag)
        # not keyboard focus: the calendar, closed with Esc while the bar is
        # hidden, hands the foreground back to the bar
        away()
        click()
        time.sleep(0.6)
        combo(0x5B, 0x4E)                       # Win+N: notifications and calendar
        time.sleep(1.2)
        away()
        time.sleep(0.8)
        key(VK_ESCAPE)
        time.sleep(1.2)
        if bar_seen():
            fails.append("calendar closed with Esc: bar came up (fg %s)" % fg_class())
    finally:
        p.kill()
        time.sleep(0.5)
    away()
    return fails, []


SHELL_TESTS["menus"] = run_menus
SHELL_TESTS["keyboard"] = run_keyboard
