"""
A window like Riot Client's main window, for run_tests.py: not maximized, a title
bar in its style, but sized a little beyond the screen on all sides. It closes by
itself after the given number of seconds.

    python tests\\bigwindow.py [seconds]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
user32.SetProcessDPIAware()
kernel32.GetModuleHandleW.restype = wt.HMODULE
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.DefWindowProcW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
user32.CreateWindowExW.restype = wt.HWND
user32.CreateWindowExW.argtypes = (wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID)
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

STYLE = 0x14C20000      # visible, clip siblings, caption, minimize box (no thick frame)
EXSTYLE = 0x00000100    # window edge


class WNDCLASS(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("proc", WNDPROC), ("cbc", ctypes.c_int), ("cbw", ctypes.c_int),
                ("inst", wt.HINSTANCE), ("icon", wt.HANDLE), ("cursor", wt.HANDLE), ("brush", wt.HANDLE),
                ("menu", wt.LPCWSTR), ("name", wt.LPCWSTR)]


def proc(h, m, w, l):
    return user32.DefWindowProcW(h, m, w, l)


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
    sw, sh = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    cb = WNDPROC(proc)
    inst = kernel32.GetModuleHandleW(None)
    # one plain color (tests tell the bar's pixels from it): RGB(30, 140, 60)
    ctypes.windll.gdi32.CreateSolidBrush.restype = wt.HANDLE
    brush = ctypes.windll.gdi32.CreateSolidBrush(0x3C8C1E)
    wc = WNDCLASS(0, cb, 0, 0, inst, None, None, brush, None, "FastBarTestBig")
    user32.RegisterClassW(ctypes.byref(wc))
    h = user32.CreateWindowExW(EXSTYLE, "FastBarTestBig", "FastBar test", STYLE,
                               -9, -1, sw + 21, sh + 11, None, None, inst, None)
    user32.SetForegroundWindow(h)
    end = time.time() + secs
    msg = wt.MSG()
    while time.time() < end:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.01)


if __name__ == "__main__":
    main()
