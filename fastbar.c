/*
 * FastBar - responsive auto-hide taskbar for Windows 10/11
 *
 * - Taskbar appears instantly when the cursor touches the screen edge
 * - Slide animation paced by DWM (DwmFlush) = follows monitor refresh rate
 * - Event driven (low-level mouse hook + WinEvent hooks): ~0% CPU when idle
 *
 * How it works:
 *   Windows "auto-hide" is enabled so the work area covers the full screen,
 *   but the real taskbar window is hidden (SW_HIDE) so Explorer's own slow
 *   auto-hide logic never fires. FastBar shows it itself and slides it with
 *   SetWindowPos(SWP_NOSENDCHANGING) once per DWM frame. When hiding, a
 *   snapshot of the taskbar is slid out in FastBar's own window, so Explorer's
 *   30 fps hide animation never runs.
 */
#define UNICODE
#define _UNICODE
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <shellapi.h>

#ifndef ABM_SETSTATE
#define ABM_SETSTATE 10
#endif
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
#ifndef EVENT_SYSTEM_FOREGROUND
#define EVENT_SYSTEM_FOREGROUND 0x0003
#endif
#ifndef EVENT_OBJECT_SHOW
#define EVENT_OBJECT_SHOW 0x8002
#endif
#ifndef EVENT_OBJECT_LOCATIONCHANGE
#define EVENT_OBJECT_LOCATIONCHANGE 0x800B
#endif
#ifndef WINEVENT_OUTOFCONTEXT
#define WINEVENT_OUTOFCONTEXT 0
#define WINEVENT_SKIPOWNPROCESS 2
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#endif

#define APP_NAME      L"FastBar"
#define WM_APP_EVAL   (WM_APP + 1)
#define WM_APP_TRAY   (WM_APP + 2)
#define WM_APP_EXPLHID (WM_APP + 3)
#define TIMER_SHOW    1
#define TIMER_HIDE    2
#define TIMER_REEVAL  3
#define TIMER_EXPLHID 4
#define TIMER_POLL    5
#define ID_ENABLE     100
#define ID_STARTUP    101
#define ID_SETTINGS   102
#define ID_RELOAD     103
#define ID_EXIT       104

/* desired states for the animation thread */
#define D_HIDDEN 0
#define D_SHOWN  1
#define D_HOLD   2   /* cursor left: swap to snapshot, wait for hide delay */

/* main-thread modes */
#define M_HIDDEN  0
#define M_SHOWN   1
#define M_LEAVING 2

/* carrier = what is currently drawn on screen */
#define C_NONE 0
#define C_REAL 1
#define C_SNAP 2
#define C_COMP 3

typedef HRESULT (WINAPI *PFN_DwmFlush)(void);
typedef HRESULT (WINAPI *PFN_DwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
typedef struct {
    DWORD dwFlags; RECT rcDestination; RECT rcSource; BYTE opacity; BOOL fVisible; BOOL fSourceClientAreaOnly;
} THUMBPROPS;                                   /* DWM_THUMBNAIL_PROPERTIES */
#define TNP_RECTDESTINATION 0x1
#define TNP_OPACITY         0x4
#define TNP_VISIBLE         0x8
#define TNP_CLIENTONLY      0x10
typedef HRESULT (WINAPI *PFN_DwmRegisterThumbnail)(HWND, HWND, HANDLE *);
typedef HRESULT (WINAPI *PFN_DwmUnregisterThumbnail)(HANDLE);
typedef HRESULT (WINAPI *PFN_DwmUpdateThumbnailProperties)(HANDLE, const THUMBPROPS *);
typedef BOOL (WINAPI *PFN_QueryFullProcessImageNameW)(HANDLE, DWORD, LPWSTR, PDWORD);

/* ---------- settings ---------- */
static int cfgShowDelay   = 0;
static int cfgHideDelay   = 300;
static int cfgShowAnimMs  = 250;
static int cfgHideAnimMs  = 180;
static int cfgEasing      = 1;
static int cfgTriggerPx   = 1;
static int cfgBlockFull   = 1;
static int cfgShowOnStart = 1;

/* ---------- shared state ---------- */
static HWND  g_hwnd, g_tb;
static HHOOK g_mouseHook;
static HWINEVENTHOOK g_fgHook, g_showHook, g_locHook;
static UINT  g_msgTaskbarCreated;
static WCHAR g_iniPath[MAX_PATH], g_exePath[MAX_PATH];
static int   g_enabled = 1;
static int   g_origAutoHide;
static int   g_mode = M_HIDDEN;
static volatile LONG g_evalPosted;
static volatile LONG g_dragInside;
static int   g_showPending;
static POINT g_hookPt;          /* last point seen by the mouse hook (main thread) */
static int   g_hookFresh;
static int   g_reevalLeft;
static void killShow(void);

#ifdef FB_DEBUG
static void dbg(const char *fmt, ...)
{
    char b[256];
    DWORD n;
    va_list ap;
    HANDLE f;
    va_start(ap, fmt);
    n = wvsprintfA(b, fmt, ap);
    va_end(ap);
    f = CreateFileA(FB_DEBUG, FILE_APPEND_DATA, 7, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, b, n, &n, NULL); CloseHandle(f); }
}
#define DBG(x) dbg x
#else
#define DBG(x)
#endif

static volatile LONG g_desired = D_SHOWN;
static volatile LONG g_animIdle = 1;
static volatile LONG g_paused = 1;      /* anim thread does nothing while paused */
static HANDLE g_evt, g_animThread;

/* geometry (physical pixels) */
static RECT g_mon;       /* monitor of the taskbar */
static RECT g_shown;     /* taskbar rect when fully visible */
static int  g_offX, g_offY; /* offset from shown to fully hidden */
static UINT g_edge = ABE_BOTTOM;

static PFN_DwmFlush pDwmFlush;
static PFN_DwmGetWindowAttribute pDwmGetAttr;
static PFN_DwmRegisterThumbnail pDwmRegThumb;
static PFN_DwmUnregisterThumbnail pDwmUnregThumb;
static PFN_DwmUpdateThumbnailProperties pDwmUpdThumb;
static PFN_QueryFullProcessImageNameW pQueryName;

/* ===================================================================== */
/* settings                                                               */
/* ===================================================================== */
static int iniInt(const WCHAR *key, int def)
{
    return (int)GetPrivateProfileIntW(APP_NAME, key, def, g_iniPath);
}
static void iniSetInt(const WCHAR *key, int v)
{
    WCHAR b[16];
    wsprintfW(b, L"%d", v);
    WritePrivateProfileStringW(APP_NAME, key, b, g_iniPath);
}

static void loadSettings(void)
{
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        static const char tmpl[] =
            "; FastBar settings - after editing, tray icon > \"Reload settings\"\r\n"
            "[FastBar]\r\n"
            "; ms to wait after the cursor touches the screen edge (0 = instant)\r\n"
            "ShowDelay=0\r\n"
            "; ms to wait after the cursor leaves the taskbar before hiding\r\n"
            "HideDelay=300\r\n"
            "; slide duration in ms when appearing / hiding (0 = no animation)\r\n"
            "ShowAnimationMs=250\r\n"
            "HideAnimationMs=180\r\n"
            "; animation curve: 0 = linear, 1 = smooth (ease-out), 2 = snappy (fast start),\r\n"
            ";                  3 = ease-in-out (slow start and end)\r\n"
            "Easing=1\r\n"
            "; how many pixels from the screen edge trigger the taskbar\r\n"
            "TriggerPixels=1\r\n"
            "; 1 = do not pop up over full-screen games / videos\r\n"
            "BlockInFullscreen=1\r\n"
            "; 1 = show the taskbar while Start / Search is open\r\n"
            "ShowWithStartMenu=1\r\n";
        HANDLE f = CreateFileW(g_iniPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(f, tmpl, sizeof tmpl - 1, &n, NULL);
            CloseHandle(f);
        }
    }
    cfgShowDelay   = iniInt(L"ShowDelay", 0);
    cfgHideDelay   = iniInt(L"HideDelay", 300);
    /* settings added in later versions: add them to an older ini */
    if (iniInt(L"ShowAnimationMs", -1) < 0) iniSetInt(L"ShowAnimationMs", 250);
    if (iniInt(L"HideAnimationMs", -1) < 0) iniSetInt(L"HideAnimationMs", 180);
    if (iniInt(L"Easing", -1) < 0) iniSetInt(L"Easing", 1);
    cfgShowAnimMs  = iniInt(L"ShowAnimationMs", 250);
    cfgHideAnimMs  = iniInt(L"HideAnimationMs", 180);
    cfgEasing      = iniInt(L"Easing", 1);
    cfgTriggerPx   = iniInt(L"TriggerPixels", 1);
    cfgBlockFull   = iniInt(L"BlockInFullscreen", 1);
    cfgShowOnStart = iniInt(L"ShowWithStartMenu", 1);
    if (cfgShowDelay < 0) cfgShowDelay = 0;
    if (cfgHideDelay < 0) cfgHideDelay = 0;
    if (cfgShowAnimMs < 0) cfgShowAnimMs = 0;
    if (cfgShowAnimMs > 2000) cfgShowAnimMs = 2000;
    if (cfgHideAnimMs < 0) cfgHideAnimMs = 0;
    if (cfgHideAnimMs > 2000) cfgHideAnimMs = 2000;
    if (cfgEasing < 0 || cfgEasing > 3) cfgEasing = 1;
    if (cfgTriggerPx < 1) cfgTriggerPx = 1;
}

/* ===================================================================== */
/* taskbar helpers                                                        */
/* ===================================================================== */
static int getAutoHide(void)
{
    APPBARDATA ab = { sizeof ab };
    return (SHAppBarMessage(ABM_GETSTATE, &ab) & ABS_AUTOHIDE) ? 1 : 0;
}
static void setAutoHide(int on)
{
    APPBARDATA ab = { sizeof ab };
    ab.hWnd = g_tb;
    ab.lParam = on ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &ab);
}

static void updateGeometry(void)
{
    RECT r;
    MONITORINFO mi = { sizeof mi };
    APPBARDATA ab = { sizeof ab };
    int w, h;
    if (!g_tb || !GetWindowRect(g_tb, &r)) return;
    w = r.right - r.left;
    h = r.bottom - r.top;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &ab)) g_edge = ab.uEdge;
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    g_mon = mi.rcMonitor;
    g_shown = r;
    g_offX = g_offY = 0;
    switch (g_edge) {
    case ABE_TOP:
        g_shown.top = g_mon.top; g_shown.bottom = g_mon.top + h; g_offY = -h; break;
    case ABE_LEFT:
        g_shown.left = g_mon.left; g_shown.right = g_mon.left + w; g_offX = -w; break;
    case ABE_RIGHT:
        g_shown.right = g_mon.right; g_shown.left = g_mon.right - w; g_offX = w; break;
    default:
        g_shown.bottom = g_mon.bottom; g_shown.top = g_mon.bottom - h; g_offY = h; break;
    }
}

static int atEdge(POINT pt)
{
    if (pt.x < g_mon.left || pt.x >= g_mon.right || pt.y < g_mon.top || pt.y >= g_mon.bottom)
        return 0;
    switch (g_edge) {
    case ABE_TOP:   return pt.y < g_mon.top + cfgTriggerPx;
    case ABE_LEFT:  return pt.x < g_mon.left + cfgTriggerPx;
    case ABE_RIGHT: return pt.x >= g_mon.right - cfgTriggerPx;
    default:        return pt.y >= g_mon.bottom - cfgTriggerPx;
    }
}

static int classIs(HWND h, const WCHAR *const *list)
{
    WCHAR cls[64];
    if (!h || !GetClassNameW(h, cls, 64)) return 0;
    for (; *list; list++)
        if (!lstrcmpW(cls, *list)) return 1;
    return 0;
}

/* windows that belong to the taskbar experience: keep the taskbar up */
static const WCHAR *const kPopupClasses[] = {
    L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"TaskListThumbnailWnd",
    L"XamlExplorerHostIslandWindow", L"TopLevelWindowForOverflowXamlIsland",
    L"NotifyIconOverflowWindow", L"Xaml_WindowedPopupClass",
    L"Windows.UI.Core.CoreWindow", L"#32768", L"ControlCenterWindow",
    L"Shell_InputSwitchTopLevelWindow", L"tooltips_class32", NULL
};
static const WCHAR *const kFgKeepClasses[] = {
    L"TaskListThumbnailWnd", L"XamlExplorerHostIslandWindow",
    L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow",
    L"Xaml_WindowedPopupClass", L"Windows.UI.Core.CoreWindow", L"#32768",
    L"ControlCenterWindow", L"Shell_InputSwitchTopLevelWindow", NULL
};
static const WCHAR *const kDesktopClasses[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", NULL };

/* Start, search etc. stay around invisibly ("cloaked") after closing */
static int isCloaked(HWND h)
{
    DWORD v = 0;
    return pDwmGetAttr && SUCCEEDED(pDwmGetAttr(h, 14 /* DWMWA_CLOAKED */, &v, sizeof v)) && v;
}

static int fgKeeps(HWND fg)
{
    return classIs(fg, kFgKeepClasses) && IsWindowVisible(fg) && !isCloaked(fg);
}

static int isStartOrSearch(HWND h)
{
    static const WCHAR *const cw[] = { L"Windows.UI.Core.CoreWindow", NULL };
    WCHAR path[MAX_PATH], *name, *p;
    DWORD pid = 0, n = MAX_PATH;
    HANDLE hp;
    int r = 0;
    if (!classIs(h, cw) || !pQueryName || isCloaked(h)) return 0;
    GetWindowThreadProcessId(h, &pid);
    hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return 0;
    if (pQueryName(hp, 0, path, &n)) {
        for (name = p = path; *p; p++) if (*p == L'\\') name = p + 1;
        r = !lstrcmpiW(name, L"StartMenuExperienceHost.exe") ||
            !lstrcmpiW(name, L"SearchHost.exe") ||
            !lstrcmpiW(name, L"SearchApp.exe") ||
            !lstrcmpiW(name, L"SearchUI.exe");
    }
    CloseHandle(hp);
    return r;
}

static int fgIsFullscreen(void)
{
    HWND f = GetForegroundWindow();
    RECT r;
    if (!f || classIs(f, kDesktopClasses) || IsZoomed(f) || !GetWindowRect(f, &r)) return 0;
    return r.left <= g_mon.left && r.top <= g_mon.top &&
           r.right >= g_mon.right && r.bottom >= g_mon.bottom;
}

/* ===================================================================== */
/* animation thread                                                       */
/* ===================================================================== */
typedef struct { HDC dc; HBITMAP bmp, old; int w, h; } Surface;

static HWND    a_snap;          /* picture of the taskbar that slides (bar sized) */
static HWND    a_cover;         /* full bar area: background + sliding picture */
static Surface a_snapS, a_coverBg;
static Surface a_trueBg;        /* what was behind the bar at the last slide-in */
static void copySurface(Surface *dst, Surface *src);
static void showOverlay(HWND h, int x, int y, int w, int hh);
static void keepOverlayOnTop(void);
static int     a_carrier = C_NONE;
static double  a_p = 0.0;       /* 0 = hidden, 1 = shown */
static DWORD   a_explorerHideUntil;

/* progress (0..1) -> how much of the bar is out. Hiding runs the same curve
   backwards, so an ease-out appear becomes an ease-in (accelerating) hide. */
static double easeOut(double t)
{
    double u = 1.0 - t;
    switch (cfgEasing) {
    case 0:  return t;                                         /* linear */
    case 2:  return 1.0 - u * u * u;                           /* snappy */
    case 3:  return t < 0.5 ? 4.0 * t * t * t                  /* ease-in-out */
                            : 1.0 - 4.0 * u * u * u;
    default: return 1.0 - u * u;                               /* smooth */
    }
}

static void posAt(double p, int *x, int *y)
{
    double k = 1.0 - easeOut(p);
    *x = g_shown.left + (int)(g_offX * k + (g_offX >= 0 ? 0.5 : -0.5));
    *y = g_shown.top  + (int)(g_offY * k + (g_offY >= 0 ? 0.5 : -0.5));
}

static void placeReal(double p)
{
    int x, y;
    posAt(p, &x, &y);
    SetWindowPos(g_tb, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
}
static void placeSnap(double p)
{
    int x, y;
    posAt(p, &x, &y);
    SetWindowPos(a_snap, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void waitFrame(void)
{
    if (!pDwmFlush || FAILED(pDwmFlush())) Sleep(4);
}

static void pumpMessages(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

/* copy the screen area (x, y, taskbar size) into a surface; off-screen parts stay black */
static void capture(Surface *s, int x, int y)
{
    int w = g_shown.right - g_shown.left, h = g_shown.bottom - g_shown.top;
    RECT cur, vis;
    HDC sdc = GetDC(NULL);
    if (w != s->w || h != s->h || !s->bmp) {
        if (s->bmp) { SelectObject(s->dc, s->old); DeleteObject(s->bmp); }
        if (!s->dc) s->dc = CreateCompatibleDC(sdc);
        s->bmp = CreateCompatibleBitmap(sdc, w, h);
        s->old = SelectObject(s->dc, s->bmp);
        s->w = w; s->h = h;
    }
    PatBlt(s->dc, 0, 0, w, h, BLACKNESS);
    SetRect(&cur, x, y, x + w, y + h);
    if (IntersectRect(&vis, &cur, &g_mon))
        BitBlt(s->dc, vis.left - x, vis.top - y, vis.right - vis.left, vis.bottom - vis.top,
               sdc, vis.left, vis.top, SRCCOPY);
    ReleaseDC(NULL, sdc);
}

/*
 * How the bar is put on screen
 * ----------------------------
 * The real taskbar never moves. Two facts about Explorer (Windows 11) force this:
 *  - moving the real bar while the cursor is over it makes Explorer process hover
 *    on every frame, stalling our SetWindowPos calls (~30 ms per frame);
 *  - in auto-hide mode Explorer only draws the bar's content once the real cursor
 *    has been over it (~80 ms later); it cannot be faked with window messages.
 * So the real bar is shown, still, at its full position as soon as the cursor
 * touches the edge (Explorer starts drawing it), while a click-through window of
 * ours on top plays the slide using a picture of the bar. When the slide is done
 * and Explorer has had time to draw, our window goes away.
 *
 *  C_SNAP: a_snap (picture of the bar, bar sized) moves. Used for hiding, and for
 *          showing again when a hide is reversed half-way.
 *  C_COMP: a_cover (full bar area) paints the background behind the bar plus the
 *          picture at the current offset. Used for a normal slide-in.
 *  a_realUnder: the real bar is shown underneath the overlay.
 */
static int     a_realUnder;
static int     a_snapValid;      /* the moving picture was taken of the full bar */
static DWORD   a_thumbFrom;      /* live thumbnail is shown from this time on */
#define REDRAW_MS 60             /* Explorer redraws a re-shown bar within a few frames */
static DWORD   a_revealAt;       /* earliest time the overlay may be removed */
#define REVEAL_MS 40             /* a few frames for Explorer to settle */

/* In auto-hide mode Explorer hides the taskbar's content by hiding this child
   window, and only shows it again ~40-90 ms after the cursor reaches the bar.
   Showing it ourselves makes the live content available within one frame. */
static void showContent(void)
{
    HWND br = FindWindowExW(g_tb, NULL, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL);
    if (br && !IsWindowVisible(br)) ShowWindow(br, SW_SHOWNA);
}

/* Opacity of the real taskbar window. While Start or Quick Settings is (or
   was) open, Windows can keep the taskbar in a z-band above every normal
   window, where no overlay of ours can cover it. So during a slide the real
   bar is made transparent instead (it keeps rendering, so its live thumbnail
   still shows the content), and opaque again when the slide is done.
   The alpha is 1, not 0: a fully transparent window no longer takes clicks. */
#define TB_INVISIBLE 1
static int g_tbLayeredByUs;
static int g_tbAlpha = 255;
static void tbAlpha(BYTE a)
{
    LONG ex;
    if (!g_tb) return;
    ex = GetWindowLongW(g_tb, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) {
        if (a == 255) { g_tbAlpha = 255; return; }   /* not layered = already opaque */
        SetWindowLongW(g_tb, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        g_tbLayeredByUs = 1;
    } else if (g_tbAlpha == a) {
        return;
    }
    SetLayeredWindowAttributes(g_tb, 0, a, LWA_ALPHA);
    g_tbAlpha = a;
}
/* While a flyout (Quick Settings, calendar...) is open, the bar is kept
   layered at full opacity. Making a window layered waits for Explorer to
   handle the style change, which takes a few frames while it is busy; then
   the bar could show above our picture for those frames when it is frozen.
   Already layered, only the alpha changes, which is immediate. */
static volatile LONG g_wantLayer;
static void tbPrelayer(void)
{
    if (!g_tb) return;
    if (GetWindowLongW(g_tb, GWL_EXSTYLE) & WS_EX_LAYERED) {
        if (g_tbAlpha != 255) tbAlpha(255);
        return;
    }
    SetWindowLongW(g_tb, GWL_EXSTYLE, GetWindowLongW(g_tb, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(g_tb, 0, 255, LWA_ALPHA);
    g_tbLayeredByUs = 1;
    g_tbAlpha = 255;
}
/* give the window its original style back (on exit / when disabled) */
static void tbAlphaRestore(void)
{
    if (g_tb && g_tbLayeredByUs) {
        SetLayeredWindowAttributes(g_tb, 0, 255, LWA_ALPHA);
        SetWindowLongW(g_tb, GWL_EXSTYLE, GetWindowLongW(g_tb, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    g_tbLayeredByUs = 0;
    g_tbAlpha = 255;
}

static void showRealFull(void)
{
    /* ShowWindow() must not be used: Explorer would snap the bar to its own
       hidden spot and run its own slow (30 fps) unhide animation. Showing it
       right at its full position is accepted as "unhidden". */
    SetWindowPos(g_tb, NULL, g_shown.left, g_shown.top, 0, 0,
                 SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
    showContent();
}

static int realAtFull(void)
{
    RECT r;
    return IsWindowVisible(g_tb) && GetWindowRect(g_tb, &r) &&
           r.left == g_shown.left && r.top == g_shown.top;
}

static DWORD a_showStart;
#define NATIVE_TIMEOUT_MS 700

/* Show the real bar underneath an overlay.
   Explorer keeps its own "hidden / shown" auto-hide state. If the bar is only
   put on screen while Explorer thinks it is hidden, the first click on it makes
   Explorer restart its own unhide (the bar jumps away and crawls back up).
   A WM_ACTIVATE message (not a real activation: focus stays where it is) makes
   Explorer switch to "shown" at once and draw its content. */
static void syncExplorerShown(void)
{
    /* posted, not sent: Explorer can take a while to handle it, and the
       animation must not wait for that */
    PostMessageW(g_tb, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(g_tb, WM_ACTIVATE, WA_INACTIVE, 0);
}

static void realUnder(int on)
{
    if (on && !a_realUnder) {
        tbAlpha(TB_INVISIBLE);             /* invisible until the overlay goes */
        showRealFull();
        syncExplorerShown();
        keepOverlayOnTop();
        a_showStart = GetTickCount();
        a_revealAt = a_showStart + REVEAL_MS;
    } else if (!on && a_realUnder) {
        ShowWindow(g_tb, SW_HIDE);
    }
    a_realUnder = on;
}

/* may the overlay go away now? */
static int realReady(void)
{
    if ((LONG)(GetTickCount() - a_revealAt) < 0) return 0;
    if ((LONG)(GetTickCount() - a_thumbFrom) < 20) return 0;
    if (realAtFull()) return 1;
    if (GetTickCount() - a_showStart > NATIVE_TIMEOUT_MS) {
        showRealFull();                        /* Explorer did not unhide: do it ourselves */
        return 1;
    }
    return 0;
}

/* The cover shows what was on screen behind the bar, with a live DWM
   thumbnail of the real bar (its actual current content) on top. */
static HANDLE  a_thumb;

static void thumbOff(void)
{
    if (a_thumb && pDwmUnregThumb) pDwmUnregThumb(a_thumb);
    a_thumb = NULL;
}

static void paintCover(void)
{
    if (a_thumb && pDwmUpdThumb) {
        THUMBPROPS tp;
        int x, y;
        posAt(a_p, &x, &y);
        ZeroMemory(&tp, sizeof tp);
        tp.dwFlags = TNP_RECTDESTINATION | TNP_OPACITY | TNP_VISIBLE | TNP_CLIENTONLY;
        tp.rcDestination.left = x - g_shown.left;
        tp.rcDestination.top = y - g_shown.top;
        tp.rcDestination.right = tp.rcDestination.left + (g_shown.right - g_shown.left);
        tp.rcDestination.bottom = tp.rcDestination.top + (g_shown.bottom - g_shown.top);
        tp.opacity = 255;
        tp.fVisible = (LONG)(GetTickCount() - a_thumbFrom) >= 0;
        tp.fSourceClientAreaOnly = FALSE;
        pDwmUpdThumb(a_thumb, &tp);
    }
    InvalidateRect(a_cover, NULL, FALSE);
    UpdateWindow(a_cover);
}

/* put the cover (background + live thumbnail at progress a_p) on screen */
static int coverOn(void)
{
    thumbOff();
    if (!pDwmRegThumb || FAILED(pDwmRegThumb(a_cover, g_tb, &a_thumb))) {
        a_thumb = NULL;
        return 0;
    }
    paintCover();                      /* thumbnail position before it shows */
    showOverlay(a_cover, g_shown.left, g_shown.top, a_coverBg.w, a_coverBg.h);
    return 1;
}

/* Explorer raises the real bar to the top of the "always on top" windows
   once the cursor is over it (and when a Quick Settings / calendar panel is
   open), which would put it above our overlay in the middle of a slide.
   Keep our overlay on top, every frame while it is shown. */
static void keepOverlayOnTop(void)
{
    HWND h = a_carrier == C_COMP ? a_cover : a_carrier == C_SNAP ? a_snap : NULL;
    if (h && IsWindowVisible(h) && GetWindow(h, GW_HWNDPREV) != NULL)
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER);
}

static void hideOverlay(void)
{
    if (a_carrier == C_COMP) {
        ShowWindow(a_cover, SW_HIDE);
        thumbOff();
        if (IsWindowVisible(a_snap)) ShowWindow(a_snap, SW_HIDE);
    } else if (a_carrier == C_SNAP) {
        ShowWindow(a_snap, SW_HIDE);
    }
}

/* Show one of our overlay windows with its new content. A window that is
   shown again first appears with what it showed last time, for one frame;
   so it is shown fully transparent, painted, and only then made opaque. */
static void showOverlay(HWND h, int x, int y, int w, int hh)
{
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    SetWindowPos(h, HWND_TOPMOST, x, y, w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    waitFrame();
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
}

/* real bar (fully shown) -> moving picture of it */
static void realToSnap(void)
{
    int x, y;
    posAt(a_p, &x, &y);
    capture(&a_snapS, x, y);
    a_snapValid = a_p >= 1.0;
    showOverlay(a_snap, x, y, a_snapS.w, a_snapS.h);
    waitFrame();                       /* picture is on screen before the real one goes */
    ShowWindow(g_tb, SW_HIDE);
    a_realUnder = 0;
    a_carrier = C_SNAP;
}

/* cursor left a fully shown bar: freeze a picture of it on top, but leave the
   real bar shown underneath (click-through picture), so that coming back is
   instant - a re-shown bar needs a few frames to be redrawn by Explorer */
static int a_explSeen;           /* Explorer's own hide already reported */
static void realToSnapHold(void)
{
    capture(&a_snapS, g_shown.left, g_shown.top);
    a_snapValid = 1;
    showOverlay(a_snap, g_shown.left, g_shown.top, a_snapS.w, a_snapS.h);
    /* With a flyout open (Quick Settings / calendar) the real bar may sit
       above our picture (higher z-band), and Explorer may start its own
       30 fps hide: make it invisible so none of that shows. It is already
       layered then (tbPrelayer), so this is immediate. Without a flyout the
       picture covers the bar anyway, and changing its style would only make
       the moving bar flash for a frame or two. */
    if (g_wantLayer) tbAlpha(TB_INVISIBLE);
    a_explSeen = 0;
    a_carrier = C_SNAP;                /* a_realUnder stays 1 */
}

static int contentShown(void)
{
    HWND br = FindWindowExW(g_tb, NULL, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL);
    return !br || IsWindowVisible(br);
}

/* start a slide-in from fully hidden */
static void startShow(void)
{
    if (cfgShowAnimMs <= 0) {
        tbAlphaRestore();
        showRealFull();
        a_realUnder = 0;
        a_p = 1.0;
        a_carrier = C_REAL;
        return;
    }
    /* The real bar can still be on screen here: after Explorer hid it by itself
       (cursor left while Quick Settings / the calendar was open) its own slow
       hide animation runs for a while. Make it invisible before taking the
       background, or the half-hidden bar ends up in the picture and shows up
       as a second taskbar behind the sliding one. */
    if (IsWindowVisible(g_tb)) {
        tbAlpha(TB_INVISIBLE);
        waitFrame();
        waitFrame();
    }
    capture(&a_coverBg, g_shown.left, g_shown.top);   /* what is behind the bar */
    copySurface(&a_trueBg, &a_coverBg);
    a_thumbFrom = GetTickCount();
    a_carrier = C_COMP;
    if (!coverOn()) {
        /* no thumbnail possible: just show the real bar */
        tbAlphaRestore();
        showRealFull();
        a_p = 1.0;
        a_carrier = C_REAL;
        return;
    }
    realUnder(1);                      /* hidden under the cover from the first frame */
}

/* the moving picture reached the top: continue with the cover so the content is
   live while Explorer finishes showing the real bar */
static void snapToCover(void)
{
    capture(&a_coverBg, g_shown.left, g_shown.top);   /* = the picture, fully shown */
    a_carrier = C_COMP;
    /* the picture stays under the cover and goes away with it (hideOverlay),
       so there is never a frame with neither of them on screen */
    if (!coverOn()) a_carrier = C_SNAP;
}

/* make dst a copy of src (same size as the bar) */
static void copySurface(Surface *dst, Surface *src)
{
    HDC sdc;
    if (!src->bmp) return;
    if (dst->w != src->w || dst->h != src->h || !dst->bmp) {
        sdc = GetDC(NULL);
        if (dst->bmp) { SelectObject(dst->dc, dst->old); DeleteObject(dst->bmp); }
        if (!dst->dc) dst->dc = CreateCompatibleDC(sdc);
        dst->bmp = CreateCompatibleBitmap(sdc, src->w, src->h);
        dst->old = SelectObject(dst->dc, dst->bmp);
        dst->w = src->w; dst->h = src->h;
        ReleaseDC(NULL, sdc);
    }
    BitBlt(dst->dc, 0, 0, src->w, src->h, src->dc, 0, 0, SRCCOPY);
}

/* A hide was reversed half-way: the moving picture may only be partly valid
   (what was off screen when it was taken is black), so continue with the
   cover and the live thumbnail instead of sliding the picture back up.
   Background: what is on screen above the picture now, plus the last known
   background behind the bar. */
static void snapToCoverPartial(void)
{
    int x, y, rows;
    int w = g_shown.right - g_shown.left, h = g_shown.bottom - g_shown.top;
    posAt(a_p, &x, &y);
    rows = y - g_shown.top;
    if (rows < 0) rows = 0;
    if (rows > h) rows = h;
    a_thumbFrom = GetTickCount();
    if (a_trueBg.bmp && a_trueBg.w == w && a_trueBg.h == h) {
        HDC sdc = GetDC(NULL);
        copySurface(&a_coverBg, &a_trueBg);
        if (rows > 0)
            BitBlt(a_coverBg.dc, 0, 0, w, rows, sdc, g_shown.left, g_shown.top, SRCCOPY);
        ReleaseDC(NULL, sdc);
    } else {
        capture(&a_coverBg, g_shown.left, g_shown.top);
    }
    a_carrier = C_COMP;
    if (coverOn()) {
        waitFrame();
        ShowWindow(a_snap, SW_HIDE);
    } else {
        a_carrier = C_SNAP;
    }
}

/* Explorer restarted its own unhide on a bar we consider shown (it jumped to
   its hidden spot): hide that under the cover until it is back */
static void maskNativeUnhide(void)
{
    tbAlpha(TB_INVISIBLE);
    a_thumbFrom = GetTickCount();
    capture(&a_coverBg, g_shown.left, g_shown.top);
    a_carrier = C_COMP;
    a_realUnder = 1;
    a_showStart = GetTickCount();
    a_revealAt = a_showStart + REVEAL_MS;
    if (!coverOn()) a_carrier = C_REAL;
}

/* slide-in reversed half-way: freeze what the cover shows into the moving picture */
static void compToSnap(void)
{
    int x, y;
    posAt(a_p, &x, &y);
    capture(&a_snapS, x, y);           /* the cover (live bar) is what is on screen */
    a_snapValid = a_p >= 1.0;
    showOverlay(a_snap, x, y, a_snapS.w, a_snapS.h);
    ShowWindow(a_cover, SW_HIDE);
    thumbOff();
    realUnder(0);
    a_carrier = C_SNAP;
}

static void animate(void)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    for (;;) {
        LONG d = g_desired;
        double target, dt;
        if (g_paused) return;

        if (d == D_HIDDEN) {
            target = 0.0;
            if (a_carrier == C_REAL) realToSnap();
            else if (a_carrier == C_COMP) compToSnap();
            else realUnder(0);
        } else {
            target = 1.0;
            if (a_carrier == C_NONE) startShow();
            if (a_carrier == C_SNAP && a_p < 1.0 && !a_snapValid) {
                /* hide reversed, picture only partly valid: go on live */
                snapToCoverPartial();
                realUnder(1);
            }
            if (d == D_HOLD) {
                /* cursor left: a fully shown bar is frozen into a picture;
                   a slide-in in progress just finishes */
                if (a_carrier == C_REAL) realToSnapHold();
                else if (a_carrier == C_COMP) realUnder(1);
            } else if (a_carrier == C_SNAP && a_p >= 1.0) {
                if (a_realUnder && realAtFull() && contentShown()) {
                    /* came back while the real bar is still in place: drop the picture */
                    tbAlphaRestore();
                    waitFrame();               /* real bar opaque before the picture goes */
                    ShowWindow(a_snap, SW_HIDE);
                    a_carrier = C_REAL;
                } else {
                    /* the real bar has to be (re)shown: keep the picture until
                       Explorer has redrawn it, then go live */
                    if (a_realUnder && !realAtFull()) a_realUnder = 0;
                    /* set before the cover appears: its thumbnail must stay
                       hidden until the re-shown bar has been redrawn */
                    a_thumbFrom = GetTickCount() + (a_realUnder ? 0 : REDRAW_MS);
                    snapToCover();
                    realUnder(1);
                }
            } else if (a_carrier == C_COMP) {
                realUnder(1);
            }
        }

        if (a_p == target) {
            if (target == 0.0 && a_carrier != C_NONE) {
                hideOverlay();
                ShowWindow(g_tb, SW_HIDE);
                a_realUnder = 0;
                a_carrier = C_NONE;
            } else if (target == 1.0 && d == D_SHOWN && a_carrier != C_REAL && a_realUnder) {
                if (realReady()) {
                    /* real bar is ready: opaque again, and its original window
                       style back (a taskbar left layered reacts more slowly) */
                    tbAlphaRestore();
                    waitFrame();
                    hideOverlay();
                    a_carrier = C_REAL;
                } else {
                    if (a_carrier == C_COMP) paintCover();
                    keepOverlayOnTop();
                    waitFrame();
                    pumpMessages();
                    continue;
                }
            }
            if (g_desired == d) return;
            QueryPerformanceCounter(&t0);
            continue;
        }

        QueryPerformanceCounter(&t1);
        dt = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        t0 = t1;
        if (dt > 50.0) dt = 50.0;  /* after a stall, do not jump */
        if (dt < 0.5) dt = 0.5;
        if (target > a_p) {
            a_p = cfgShowAnimMs <= 0 ? 1.0 : a_p + dt / cfgShowAnimMs;
            if (a_p > 1.0) a_p = 1.0;
        } else {
            a_p = cfgHideAnimMs <= 0 ? 0.0 : a_p - dt / cfgHideAnimMs;
            if (a_p < 0.0) a_p = 0.0;
        }

        if (a_carrier == C_SNAP) placeSnap(a_p);
        else if (a_carrier == C_COMP) paintCover();
        keepOverlayOnTop();
        waitFrame();
        pumpMessages();
    }
}

/* Picture of the fully shown bar, taken when the cursor leaves it while the
   bar stays up (e.g. Quick Settings / the calendar is open). If Explorer then
   starts hiding the bar by itself, this picture lets FastBar play its own
   hide (hide delay + smooth slide) instead of Explorer's fast 30 fps one. */
static Surface a_precap;
static DWORD   a_precapAt;
static int     a_precapOk;
static volatile LONG g_precapReq;
#define PRECAP_MAX_AGE_MS 2000

static void precapture(void)
{
    if (a_carrier == C_REAL && a_p >= 1.0 && realAtFull() && contentShown()) {
        capture(&a_precap, g_shown.left, g_shown.top);
        a_precapAt = GetTickCount();
        a_precapOk = 1;
        DBG(("precap ok t=%lu\r\n", a_precapAt));
    } else {
        DBG(("precap skip carrier=%d full=%d content=%d\r\n", a_carrier, realAtFull(), contentShown()));
    }
}

/* keep things consistent if Explorer moves/shows the taskbar on its own */
static void guard(void)
{
    if (g_paused || !g_tb) return;
    if (a_carrier == C_NONE && g_desired == D_HIDDEN) {
        /* let Explorer finish its own hide animation before hiding the window */
        if (IsWindowVisible(g_tb) && (LONG)(GetTickCount() - a_explorerHideUntil) >= 0)
            ShowWindow(g_tb, SW_HIDE);
    } else if (a_carrier == C_SNAP && a_realUnder && g_desired == D_HOLD && !a_explSeen) {
        /* bar frozen into a picture (cursor away, flyout open): if Explorer
           starts hiding the (invisible) real bar, tell the main thread, which
           then hides the bar the normal FastBar way */
        RECT r;
        if (IsWindowVisible(g_tb) && GetWindowRect(g_tb, &r) &&
            (r.left - g_shown.left) * g_offX + (r.top - g_shown.top) * g_offY > 0) {
            a_explSeen = 1;
            DBG(("guard: explorer hides frozen bar t=%lu\r\n", GetTickCount()));
            PostMessageW(g_hwnd, WM_APP_EXPLHID, 1, 0);
        }
    } else if (a_carrier == C_REAL && a_p >= 1.0 && g_desired == D_SHOWN) {
        RECT r;
        if (g_wantLayer) tbPrelayer();
        else tbAlphaRestore();
        if (!IsWindowVisible(g_tb)) showRealFull();
        else if (GetWindowRect(g_tb, &r) && (r.left != g_shown.left || r.top != g_shown.top)) {
            long dx = r.left - g_shown.left, dy = r.top - g_shown.top;
            long along = dx * g_offX + dy * g_offY;          /* > 0: towards hidden */
            long full = g_offX * g_offX + g_offY * g_offY;
            DBG(("guard moved dx=%ld dy=%ld along=%ld full=%ld precap=%d age=%lu t=%lu\r\n", dx, dy, along, full,
                 a_precapOk, GetTickCount() - a_precapAt, GetTickCount()));
            if (along * 4 >= full * 3) {
                /* jumped (almost) all the way: Explorer restarts its own unhide */
                maskNativeUnhide();
                SetEvent(g_evt);
            } else if (along > 0 && a_precapOk &&
                       GetTickCount() - a_precapAt < PRECAP_MAX_AGE_MS &&
                       a_precap.w == g_shown.right - g_shown.left &&
                       a_precap.h == g_shown.bottom - g_shown.top) {
                /* Explorer started hiding the bar by itself: cover it with the
                   picture taken while it was still fully shown, make the real
                   bar invisible, and hide it the normal FastBar way */
                a_precapOk = 0;
                copySurface(&a_snapS, &a_precap);
                a_snapValid = 1;
                showOverlay(a_snap, g_shown.left, g_shown.top, a_snapS.w, a_snapS.h);
                tbAlpha(TB_INVISIBLE);
                a_realUnder = 0;
                a_carrier = C_SNAP;
                a_p = 1.0;
                a_explorerHideUntil = GetTickCount() + 600;
                InterlockedExchange(&g_desired, D_HOLD);
                PostMessageW(g_hwnd, WM_APP_EXPLHID, 1, 0);
            } else if (along > 0) {
                /* Explorer is hiding the bar by itself (e.g. the cursor went into
                   Quick Settings / the calendar). Its content is already sliding
                   away, so putting the window back would leave an invisible bar
                   that still eats clicks: go along with it instead. */
                a_carrier = C_NONE;
                a_p = 0.0;
                a_explorerHideUntil = GetTickCount() + 600;
                InterlockedExchange(&g_desired, D_HIDDEN);
                PostMessageW(g_hwnd, WM_APP_EXPLHID, 0, 0);
            } else {
                placeReal(1.0);
            }
        }
    }
}

static LRESULT CALLBACK SnapProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (h == a_cover) {
            /* background; the live thumbnail of the bar is drawn on top by DWM */
            if (a_coverBg.dc) BitBlt(dc, 0, 0, a_coverBg.w, a_coverBg.h, a_coverBg.dc, 0, 0, SRCCOPY);
        } else if (a_snapS.dc) {
            BitBlt(dc, 0, 0, a_snapS.w, a_snapS.h, a_snapS.dc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI AnimThread(LPVOID unused)
{
    WNDCLASSW wc = { 0 };
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    wc.lpfnWndProc = SnapProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"FastBarSnapshot";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    /* both overlays are click-through (layered + transparent): the mouse goes
       to the real bar underneath, which is what makes Explorer draw it */
    a_snap = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                             WS_EX_LAYERED | WS_EX_TRANSPARENT,
                             wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
                             NULL, NULL, wc.hInstance, NULL);
    a_cover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                              WS_EX_LAYERED | WS_EX_TRANSPARENT,
                              wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
                              NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(a_snap, 0, 255, LWA_ALPHA);
    SetLayeredWindowAttributes(a_cover, 0, 255, LWA_ALPHA);
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &g_evt, FALSE, INFINITE, QS_ALLINPUT);
        pumpMessages();
        if (r == WAIT_OBJECT_0) {
            InterlockedExchange(&g_animIdle, 0);
            animate();
            if (InterlockedExchange(&g_precapReq, 0)) precapture();
            guard();
            InterlockedExchange(&g_animIdle, 1);
        }
    }
    return 0;
}

static void setDesired(LONG d)
{
    InterlockedExchange(&g_desired, d);
    SetEvent(g_evt);
}

/* hand the taskbar back to Windows (fully visible, real window) */
static void restoreTaskbar(void)
{
    int i;
    InterlockedExchange(&g_paused, 1);
    SetEvent(g_evt);
    for (i = 0; i < 100 && !g_animIdle; i++) Sleep(5);
    if (a_snap) ShowWindow(a_snap, SW_HIDE);
    if (a_cover) ShowWindow(a_cover, SW_HIDE);
    thumbOff();
    if (g_tb) {
        a_p = 1.0;
        a_realUnder = 0;
        tbAlphaRestore();
        showRealFull();
        a_carrier = C_REAL;
        setAutoHide(g_origAutoHide);
    }
    iniSetInt(L"Running", 0);
}

/* ===================================================================== */
/* main thread logic                                                      */
/* ===================================================================== */
static int cursorInside(POINT pt)
{
    RECT r = g_shown;
    HWND under;
    InflateRect(&r, 1, 1);
    if (PtInRect(&r, pt) || atEdge(pt)) return 1;
    if (g_dragInside && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return 1;
    under = WindowFromPoint(pt);
    if (under && under != a_snap) {
        HWND root = GetAncestor(under, GA_ROOT);
        static const WCHAR *const cw[] = { L"Windows.UI.Core.CoreWindow", NULL };
        if (root != a_snap && classIs(root, kPopupClasses) && !isCloaked(root) &&
            /* Start/search linger while closing: only count them while active */
            (!classIs(root, cw) || root == GetForegroundWindow()))
            return 1;
    }
    return 0;
}

/* the cursor is on the bar itself (not on one of its popups) */
static int cursorOnBar(POINT pt)
{
    RECT r = g_shown;
    InflateRect(&r, 1, 1);
    if (PtInRect(&r, pt) || atEdge(pt)) return 1;
    return g_dragInside && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
}

/* Explorer hid the bar by itself while Quick Settings / the calendar was open
   and FastBar took over the hide: until the bar is shown again, only the bar
   itself brings it back (the open flyout would just make Explorer hide it again) */
static int g_explHid;
static int g_onBar;
static DWORD g_leaveAt;     /* when the cursor last left the bar */
/* the cursor is away but an open flyout (Quick Settings, calendar, jump list...)
   keeps the bar up: it is frozen into a picture (D_HOLD) while in M_SHOWN */
static int g_frozen;

/* should the bar stay up / come back? */
static int wantShown(POINT pt, HWND fg)
{
    if (g_explHid) return cursorOnBar(pt);
    return cursorInside(pt) || fgKeeps(fg);
}

static void killShow(void) { KillTimer(g_hwnd, TIMER_SHOW); g_showPending = 0; }

static void showNow(void)
{
    killShow();
    KillTimer(g_hwnd, TIMER_HIDE);
    g_mode = M_SHOWN;
    g_frozen = 0;
    g_explHid = 0;
    setDesired(D_SHOWN);
}

/* Windows' UIPI: while a window of a program running as administrator (Task
   Manager, an elevated console, ...) is in the foreground, our low-level mouse
   hook gets no events at all. Then the cursor is polled instead (GetCursorPos
   still works), only for as long as such a window is in front. */
static DWORD integrityOf(HANDLE proc)
{
    HANDLE tok;
    DWORD il = 0;
    if (OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
        union { SID_AND_ATTRIBUTES label; BYTE raw[64]; } buf;   /* TOKEN_MANDATORY_LABEL */
        DWORD n;
        if (GetTokenInformation(tok, (TOKEN_INFORMATION_CLASS)25 /* TokenIntegrityLevel */,
                                &buf, sizeof buf, &n)) {
            PSID sid = buf.label.Sid;
            il = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        CloseHandle(tok);
    } else {
        il = 0xFFFFFFFF;                    /* cannot even look: higher than us */
    }
    return il;
}

static int fgAboveUs(HWND fg)
{
    static DWORD ownIL;
    DWORD pid = 0, il;
    HANDLE p;
    if (!fg) return 0;
    if (!ownIL) ownIL = integrityOf(GetCurrentProcess());
    GetWindowThreadProcessId(fg, &pid);
    p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 1;
    il = integrityOf(p);
    CloseHandle(p);
    return il > ownIL;
}

static HWND g_pollFor;
static int  g_polling;
static void updatePolling(HWND fg)
{
    int need;
    if (fg == g_pollFor) return;          /* same foreground window: nothing changed */
    g_pollFor = fg;
    need = g_enabled && fgAboveUs(fg);
    if (need && !g_polling) SetTimer(g_hwnd, TIMER_POLL, 16, NULL);
    if (!need && g_polling) KillTimer(g_hwnd, TIMER_POLL);
    g_polling = need;
}

static void evaluate(void)
{
    POINT pt;
    HWND fg;
    if (!g_enabled || !g_tb) return;
    /* the LL hook runs before the cursor really moves, so GetCursorPos would
       still report the old spot: prefer the point the hook saw */
    if (g_hookFresh) { pt = g_hookPt; g_hookFresh = 0; }
    else GetCursorPos(&pt);
    fg = GetForegroundWindow();
    updatePolling(fg);
    DBG(("eval pt=%ld,%ld mode=%d mon=%ld,%ld,%ld,%ld edge=%d at=%d full=%d\r\n", pt.x, pt.y, g_mode,
         g_mon.left, g_mon.top, g_mon.right, g_mon.bottom, g_edge, atEdge(pt), fgIsFullscreen()));

    if (g_mode == M_HIDDEN) {
        if (cfgShowOnStart && isStartOrSearch(fg)) { showNow(); return; }
        if (atEdge(pt) && !(cfgBlockFull && fgIsFullscreen())) {
            if (cfgShowDelay <= 0) showNow();
            else if (!g_showPending) { g_showPending = 1; SetTimer(g_hwnd, TIMER_SHOW, cfgShowDelay, NULL); }
        } else {
            killShow();
        }
        return;
    }

#ifdef FB_DEBUG
    {
        char c1[64] = "", c2[64] = "";
        HWND r = GetAncestor(WindowFromPoint(pt), GA_ROOT);
        GetClassNameA(fg, c1, 64); GetClassNameA(r, c2, 64);
        DBG(("  shown-branch inside=%d fgKeeps=%d fg=%s under=%s cloaked=%d\r\n", cursorInside(pt), fgKeeps(fg), c1, c2, isCloaked(r)));
    }
#endif
    if (g_mode == M_SHOWN) {
        /* cursor just left the bar: keep a picture of it, in case Explorer
           starts hiding it by itself (see guard) */
        int on = cursorOnBar(pt);
        if (!on && g_onBar) {
            g_leaveAt = GetTickCount();
            InterlockedExchange(&g_precapReq, 1);
            SetEvent(g_evt);
        }
        g_onBar = on;
    }
    {
        /* a flyout is open: prepare the bar for a quick freeze (tbPrelayer) */
        LONG wl = g_mode == M_SHOWN && fgKeeps(fg);
        if (wl != g_wantLayer) {
            InterlockedExchange(&g_wantLayer, wl);
            SetEvent(g_evt);
        }
    }
    if (wantShown(pt, fg)) {
        if (g_mode != M_SHOWN) {
            KillTimer(g_hwnd, TIMER_HIDE);
            g_mode = M_SHOWN;
            g_explHid = 0;
            g_frozen = 0;
            g_onBar = cursorOnBar(pt);
            setDesired(D_SHOWN);
        } else {
            /* kept up only by an open flyout, cursor elsewhere: freeze the
               bar into a picture; live again once the cursor is back */
            int frz = !cursorInside(pt);
            if (frz != g_frozen) {
                g_frozen = frz;
                setDesired(frz ? D_HOLD : D_SHOWN);
            }
        }
    } else if (g_mode == M_SHOWN) {
        g_frozen = 0;
        g_mode = M_LEAVING;
        if (cfgHideDelay <= 0) { g_mode = M_HIDDEN; setDesired(D_HIDDEN); }
        else { setDesired(D_HOLD); SetTimer(g_hwnd, TIMER_HIDE, cfgHideDelay, NULL); }
    }
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_enabled) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        int relevant;
        g_hookPt = m->pt;
        g_hookFresh = 1;
        if (wp == WM_LBUTTONDOWN)
            g_dragInside = (g_mode != M_HIDDEN) && PtInRect(&g_shown, m->pt);
        /* while hidden only the screen edge matters: skip everything else */
        relevant = (g_mode != M_HIDDEN) || atEdge(m->pt) || cfgShowDelay > 0;
        if (relevant && !InterlockedExchange(&g_evalPosted, 1))
            PostMessageW(g_hwnd, WM_APP_EVAL, 0, 0);
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK hk, DWORD ev, HWND h, LONG obj, LONG child,
                                  DWORD thread, DWORD time)
{
    (void)hk; (void)child; (void)thread; (void)time;
    if (ev == EVENT_SYSTEM_FOREGROUND) {
        PostMessageW(g_hwnd, WM_APP_EVAL, 0, 0);
        /* Start/search close with an animation; look again once it is gone */
        g_reevalLeft = 3;
        SetTimer(g_hwnd, TIMER_REEVAL, 150, NULL);
    } else if (h == g_tb && obj == OBJID_WINDOW && !g_paused && g_animIdle) {
        SetEvent(g_evt);   /* let the anim thread re-check (guard) */
    }
}

static void unhookShell(void)
{
    if (g_showHook) { UnhookWinEvent(g_showHook); g_showHook = NULL; }
    if (g_locHook)  { UnhookWinEvent(g_locHook);  g_locHook = NULL; }
}

static void hookShell(void)
{
    DWORD pid = 0;
    unhookShell();
    if (!g_tb) return;
    GetWindowThreadProcessId(g_tb, &pid);
    g_showHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, WinEventProc,
                                 pid, 0, WINEVENT_OUTOFCONTEXT);
    g_locHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL,
                                WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
}

/* the hook is removed whenever the main thread may block, so the cursor never stutters */
static void mouseHook(int on)
{
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = NULL; }
    if (on) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(NULL), 0);
}

/* take control of the taskbar */
static void engage(void)
{
    g_tb = FindWindowW(L"Shell_TrayWnd", NULL);
    RECT r;
    int i;
    mouseHook(0);
    if (!g_tb) return;
    g_tbLayeredByUs = 0;               /* (possibly) a new taskbar window */
    g_tbAlpha = 255;
    updateGeometry();
    hookShell();
    GetWindowRect(g_tb, &r);
    /* fully visible right now? then slide it away nicely, else just hide it */
    if (IsWindowVisible(g_tb) && r.left == g_shown.left && r.top == g_shown.top) {
        a_carrier = C_REAL;
        a_p = 1.0;
    } else {
        ShowWindow(g_tb, SW_HIDE);
        a_carrier = C_NONE;
        a_p = 0.0;
    }
    g_mode = M_HIDDEN;
    g_explHid = 0;
    g_frozen = 0;
    a_precapOk = 0;
    InterlockedExchange(&g_desired, D_HIDDEN);
    InterlockedExchange(&g_paused, 0);
    iniSetInt(L"Running", 1);
    SetEvent(g_evt);
    if (!getAutoHide()) {
        /* turn on auto-hide (full-screen work area) only after our slide-out,
           otherwise Explorer starts its own hide animation first */
        for (i = 0; i < 200 && a_carrier != C_NONE; i++) Sleep(5);
        setAutoHide(1);
        updateGeometry();
    }
    mouseHook(1);
}

static void setEnabled(int on)
{
    g_enabled = on;
    g_pollFor = NULL;                       /* re-check polling on the next evaluate */
    if (!on && g_polling) { KillTimer(g_hwnd, TIMER_POLL); g_polling = 0; }
    killShow();
    KillTimer(g_hwnd, TIMER_HIDE);
    if (on) engage();
    else { mouseHook(0); restoreTaskbar(); }
}

/* ---------- tray icon ---------- */
static HICON makeIcon(void)
{
    int s = GetSystemMetrics(SM_CXSMICON), x, y;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER) } };
    DWORD *px;
    ICONINFO ii = { TRUE };
    HICON ic;
    HDC dc = GetDC(NULL);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    ii.hbmColor = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ii.hbmMask = CreateBitmap(s, s, 1, 1, NULL);
    ReleaseDC(NULL, dc);
    for (y = 0; y < s; y++)
        for (x = 0; x < s; x++) {
            DWORD c = 0;
            int m = s / 8 + 1;
            int frame = (x >= 0 && x < s && y >= m && y < s - m) &&
                        (x < 2 || x >= s - 2 || y < m + 2 || y >= s - m - 2);
            int bar = x >= 3 && x < s - 3 && y >= s - m - 2 - s / 4 && y < s - m - 3;
            int arrow = (y >= m + 3 && y < m + 3 + s / 4) &&
                        (x >= s / 2 - (y - m - 3) - 1 && x <= s / 2 + (y - m - 3));
            if (frame) c = 0xFFFFFFFF;
            else if (bar) c = 0xFF3C9BFF;
            else if (arrow) c = 0xFFFFFFFF;
            px[y * s + x] = c;
        }
    ic = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return ic;
}

static HICON g_icon;
static void trayAdd(void)
{
    NOTIFYICONDATAW nd = { sizeof nd };
    nd.hWnd = g_hwnd;
    nd.uID = 1;
    nd.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nd.uCallbackMessage = WM_APP_TRAY;
    nd.hIcon = g_icon;
    lstrcpyW(nd.szTip, L"FastBar");
    Shell_NotifyIconW(NIM_ADD, &nd);
}
static void trayRemove(void)
{
    NOTIFYICONDATAW nd = { sizeof nd };
    nd.hWnd = g_hwnd;
    nd.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nd);
}

static int startupEnabled(void)
{
    HKEY k;
    int r = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        r = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return r;
}
static void setStartup(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR v[MAX_PATH + 2];
        wsprintfW(v, L"\"%s\"", g_exePath);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (BYTE *)v, (lstrlenW(v) + 1) * sizeof(WCHAR));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

static void showMenu(void)
{
    POINT pt;
    HMENU m = CreatePopupMenu();
    int cmd;
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_ENABLE, L"Enabled");
    AppendMenuW(m, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Edit settings...");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"Reload settings");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case ID_ENABLE:   setEnabled(!g_enabled); break;
    case ID_STARTUP:  setStartup(!startupEnabled()); break;
    case ID_SETTINGS: ShellExecuteW(NULL, L"open", L"notepad.exe", g_iniPath, NULL, SW_SHOWNORMAL); break;
    case ID_RELOAD:   loadSettings(); break;
    case ID_EXIT:     DestroyWindow(g_hwnd); break;
    }
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_APP_EXPLHID:          /* Explorer hid the bar on its own */
        if (w) {
            /* the bar is covered by its picture (frozen, or taken over by
               guard): hide it the normal way, the hide delay counted from
               when the cursor left the bar. Already leaving: nothing to do. */
            LONG rest = cfgHideDelay - (LONG)(GetTickCount() - g_leaveAt);
            if (g_mode != M_SHOWN) return 0;
            killShow();
            g_explHid = 1;
            g_frozen = 0;
            g_onBar = 0;
            DBG(("explhid takeover rest=%ld\r\n", rest));
            if (rest <= 0) { g_mode = M_HIDDEN; g_explHid = 0; setDesired(D_HIDDEN); }
            else { g_mode = M_LEAVING; setDesired(D_HOLD); SetTimer(h, TIMER_HIDE, (UINT)rest, NULL); }
            return 0;
        }
        killShow();
        KillTimer(h, TIMER_HIDE);
        g_explHid = 0;
        g_mode = M_HIDDEN;
        SetTimer(h, TIMER_EXPLHID, 650, NULL);  /* then hide the window for real */
        return 0;
    case WM_APP_EVAL:
        InterlockedExchange(&g_evalPosted, 0);
        evaluate();
        return 0;
    case WM_TIMER:
        if (w == TIMER_POLL) {              /* elevated window in front: no hook events */
            g_hookFresh = 0;
            evaluate();
            return 0;
        }
        KillTimer(h, w);
        if (w == TIMER_SHOW) g_showPending = 0;
        if (w == TIMER_EXPLHID) {
            if (g_mode == M_HIDDEN) SetEvent(g_evt);   /* guard hides the window */
            return 0;
        }
        if (w == TIMER_REEVAL) {
            if (--g_reevalLeft > 0) SetTimer(h, TIMER_REEVAL, 150, NULL);
            g_hookFresh = 0;
            evaluate();
            return 0;
        }
        if (w == TIMER_SHOW) {
            POINT pt;
            GetCursorPos(&pt);
            if (g_mode == M_HIDDEN && atEdge(pt)) showNow();
        } else if (w == TIMER_HIDE && g_mode == M_LEAVING) {
            POINT pt;
            GetCursorPos(&pt);
            if (wantShown(pt, GetForegroundWindow())) {
                g_mode = M_SHOWN;
                g_explHid = 0;
                g_frozen = 0;
                g_onBar = cursorOnBar(pt);
                setDesired(D_SHOWN);
            } else {
                g_explHid = 0;
                g_mode = M_HIDDEN;
                setDesired(D_HIDDEN);
            }
        }
        return 0;
    case WM_APP_TRAY:
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP) showMenu();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED:
        if (g_enabled && g_tb) {
            updateGeometry();
            if (g_mode != M_HIDDEN) setDesired(g_desired); /* re-place */
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w) restoreTaskbar();
        return 0;
    case WM_DESTROY:
        if (g_enabled) restoreTaskbar();
        trayRemove();
        PostQuitMessage(0);
        return 0;
    }
    if (m == g_msgTaskbarCreated && m) {   /* Explorer restarted */
        trayAdd();
        if (g_enabled) {
            mouseHook(0);
            InterlockedExchange(&g_paused, 1);
            Sleep(300);
            engage();
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *e)
{
    (void)e;
    if (g_tb) {
        tbAlphaRestore();
        ShowWindow(g_tb, SW_SHOWNOACTIVATE);
        SetWindowPos(g_tb, NULL, g_shown.left, g_shown.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        setAutoHide(g_origAutoHide);
        iniSetInt(L"Running", 0);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void setDpiAware(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *PFN_Ctx)(HANDLE);
    typedef BOOL (WINAPI *PFN_Old)(void);
    PFN_Ctx ctx = (PFN_Ctx)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (ctx && ctx((HANDLE)(LONG_PTR)-4)) return;   /* per-monitor v2 */
    {
        PFN_Old old = (PFN_Old)GetProcAddress(u, "SetProcessDPIAware");
        if (old) old();
    }
}

/* ---------- watchdog ----------
   FastBar hides the real taskbar window. If FastBar is killed (End task) it
   cannot put it back, and the taskbar would stay hidden. So a second, tiny
   FastBar process waits (no CPU: it sleeps on the process handle) for the main
   one to end. If it ended without restoring the taskbar ("Running=1" still in
   the ini), the watchdog restores it.
   It is started through a short-lived middle process so that it is not a child
   of FastBar, and "End process tree" on FastBar does not take it down too. */
static const WCHAR *argAfter(const WCHAR *line, const WCHAR *flag)
{
    int n = lstrlenW(flag);
    for (; *line; line++) {
        int i;
        for (i = 0; i < n && line[i] == flag[i]; i++) ;
        if (i == n) return line + n;
    }
    return NULL;
}

static DWORD parseNum(const WCHAR *s)
{
    DWORD v = 0;
    while (*s == L' ') s++;
    while (*s >= L'0' && *s <= L'9') v = v * 10 + (DWORD)(*s++ - L'0');
    return v;
}

static int launchSelf(const WCHAR *mode, DWORD pid)
{
    WCHAR line[MAX_PATH + 64];
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    wsprintfW(line, L"\"%s\" %s %lu", g_exePath, mode, pid);
    if (!CreateProcessW(g_exePath, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static void runWatchdog(DWORD pid)
{
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return;
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   /* tiny while waiting */
    WaitForSingleObject(p, INFINITE);
    CloseHandle(p);
    if (!iniInt(L"Running", 0)) return;        /* FastBar cleaned up itself */
    g_tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (g_tb) {
        LONG ex = GetWindowLongW(g_tb, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) {              /* killed in the middle of a slide */
            SetLayeredWindowAttributes(g_tb, 0, 255, LWA_ALPHA);
            SetWindowLongW(g_tb, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        }
        ShowWindow(g_tb, SW_SHOWNOACTIVATE);
        setAutoHide(iniInt(L"OriginalAutoHide", 0));
    }
    iniSetInt(L"Running", 0);
}

/* Start the guard: FastBarGuard.exe next to FastBar.exe (shows up under its own
   name in Task Manager); without it, a second copy of FastBar does the job. */
static void startGuard(void)
{
    WCHAR guard[MAX_PATH], line[MAX_PATH + 48], *slash, *p;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    lstrcpyW(guard, g_exePath);
    slash = guard;
    for (p = guard; *p; p++) if (*p == L'\\') slash = p + 1;
    lstrcpyW(slash, L"FastBarGuard.exe");
    if (GetFileAttributesW(guard) != INVALID_FILE_ATTRIBUTES) {
        wsprintfW(line, L"\"%s\" --spawn %lu", guard, GetCurrentProcessId());
        if (CreateProcessW(guard, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
    launchSelf(L"--spawn-watch", GetCurrentProcessId());
}

static void setIniPath(void)
{
    WCHAR *slash, *p;
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    lstrcpyW(g_iniPath, g_exePath);
    slash = g_iniPath;
    for (p = g_iniPath; *p; p++) if (*p == L'\\') slash = p + 1;
    lstrcpyW(slash, L"FastBar.ini");
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    HANDLE mtx;
    const WCHAR *arg;
    (void)prev; (void)cmd; (void)show;

    setIniPath();
    if ((arg = argAfter(GetCommandLineW(), L" --watch ")) != NULL) {
        runWatchdog(parseNum(arg));
        return 0;
    }
    if ((arg = argAfter(GetCommandLineW(), L" --spawn-watch ")) != NULL) {
        launchSelf(L"--watch", parseNum(arg));   /* and exit: the watchdog has no parent */
        return 0;
    }

    mtx = CreateMutexW(NULL, TRUE, L"Local\\FastBarSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"FastBar is already running (see its icon in the system tray).", APP_NAME, MB_ICONINFORMATION);
        return 0;
    }

    setDpiAware();
    SetUnhandledExceptionFilter(CrashHandler);

    loadSettings();

    {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        pDwmFlush = (PFN_DwmFlush)GetProcAddress(dwm, "DwmFlush");
        pDwmGetAttr = (PFN_DwmGetWindowAttribute)GetProcAddress(dwm, "DwmGetWindowAttribute");
        pDwmRegThumb = (PFN_DwmRegisterThumbnail)GetProcAddress(dwm, "DwmRegisterThumbnail");
        pDwmUnregThumb = (PFN_DwmUnregisterThumbnail)GetProcAddress(dwm, "DwmUnregisterThumbnail");
        pDwmUpdThumb = (PFN_DwmUpdateThumbnailProperties)GetProcAddress(dwm, "DwmUpdateThumbnailProperties");
    }
    pQueryName = (PFN_QueryFullProcessImageNameW)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                               "QueryFullProcessImageNameW");

    /* remember the user's own auto-hide setting (survives a crash via the ini) */
    if (iniInt(L"Running", 0)) g_origAutoHide = iniInt(L"OriginalAutoHide", 0);
    else { g_origAutoHide = getAutoHide(); iniSetInt(L"OriginalAutoHide", g_origAutoHide); }

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"FastBarMain";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hi, NULL);

    g_icon = makeIcon();
    trayAdd();

    g_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_animThread = CreateThread(NULL, 64 * 1024, AnimThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    while (!a_snap) Sleep(1);

    engage();
    startGuard();
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc,
                               0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    /* drop startup pages: keeps the resident footprint tiny */
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_mouseHook);
    UnhookWinEvent(g_fgHook);
    unhookShell();
    ReleaseMutex(mtx);
    return 0;
}
