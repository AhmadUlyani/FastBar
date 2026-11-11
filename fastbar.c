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
 *   auto-hide logic never fires. FastBar shows it itself, standing still at
 *   its place, and plays every slide (in and out) in a see-through window of
 *   its own on top, with a live DWM thumbnail of the real taskbar, once per
 *   DWM frame. Explorer's 30 fps animations never show.
 */
#define UNICODE
#define _UNICODE
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

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
#define TIMER_TAP     6
#define TIMER_EXPLSYNC 7
#define TIMER_GEOM    8
#define WM_APP_GEOM   (WM_APP + 4)
#define ID_ENABLE     100
#define ID_STARTUP    101
#define ID_SETTINGS   102
#define ID_RELOAD     103
#define ID_EXIT       104
#define ID_TRANSPARENT 105

/* desired states for the animation thread */
#define D_HIDDEN 0
#define D_SHOWN  1
#define D_HOLD   2   /* cursor left: the bar stays up (live) for now, see animate() */

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
static volatile LONG cfgTransparent = 0;  /* see-through taskbar background (FastBarTap.dll) */
static DWORD cfgColor = 0;               /* its color, ARGB (0 = fully clear) */

/* ---------- shared state ---------- */
static HWND  g_hwnd, g_tb;
static HHOOK g_mouseHook;
static HWINEVENTHOOK g_fgHook, g_showHook, g_locHook, g_menuHook;
static UINT  g_msgTaskbarCreated;
static WCHAR g_iniPath[MAX_PATH], g_exePath[MAX_PATH];
static int   g_enabled = 1;
static int   g_origAutoHide;
static int   g_mode = M_HIDDEN;
static volatile LONG g_evalPosted;
static volatile LONG g_dragInside;
static int   g_showPending;     /* show delay counting (or over, see inBarArea) */
static DWORD g_showSince;       /* ... since this tick */
static POINT g_hookPt;          /* last point seen by the mouse hook (main thread) */
static int   g_hookFresh;
static int   g_reevalLeft;
static void killShow(void);
static void tapPin(int on);
static int cursorOnBar(POINT pt);

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
            "ShowWithStartMenu=1\r\n"
            "; 1 = see-through taskbar background (also in the tray menu)\r\n"
            "TransparentTaskbar=0\r\n"
            "; its color as AARRGGBB hex: 00000000 = fully clear, 66000000 = dark tint\r\n"
            "TaskbarColor=00000000\r\n";
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
    if (iniInt(L"TransparentTaskbar", -1) < 0) iniSetInt(L"TransparentTaskbar", 0);
    {
        WCHAR c[32];
        const WCHAR *q;
        GetPrivateProfileStringW(APP_NAME, L"TaskbarColor", L"", c, 32, g_iniPath);
        if (!c[0]) WritePrivateProfileStringW(APP_NAME, L"TaskbarColor", L"00000000", g_iniPath);
        cfgColor = 0;
        for (q = c; *q == L' ' || *q == L'#'; q++) ;
        for (; *q; q++) {
            int d = *q >= L'0' && *q <= L'9' ? *q - L'0' : *q >= L'a' && *q <= L'f' ? *q - L'a' + 10 :
                    *q >= L'A' && *q <= L'F' ? *q - L'A' + 10 : -1;
            if (d < 0) break;
            cfgColor = cfgColor * 16 + (DWORD)d;
        }
    }
    cfgShowAnimMs  = iniInt(L"ShowAnimationMs", 250);
    cfgHideAnimMs  = iniInt(L"HideAnimationMs", 180);
    cfgEasing      = iniInt(L"Easing", 1);
    cfgTriggerPx   = iniInt(L"TriggerPixels", 1);
    cfgBlockFull   = iniInt(L"BlockInFullscreen", 1);
    cfgShowOnStart = iniInt(L"ShowWithStartMenu", 1);
    InterlockedExchange(&cfgTransparent, iniInt(L"TransparentTaskbar", 0) ? 1 : 0);
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
/* the auto-hide state we set last: a different state means the user changed
   it (Settings > Taskbar), see autoHideChangedByUser() */
static int g_lastAutoHide = -1, g_settingAutoHide;
static void setAutoHide(int on)
{
    APPBARDATA ab = { sizeof ab };
    ab.hWnd = g_tb;
    ab.lParam = on ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
    g_lastAutoHide = on;
    g_settingAutoHide = 1;      /* Explorer's broadcasts can arrive during the call */
    SHAppBarMessage(ABM_SETSTATE, &ab);
    g_settingAutoHide = 0;
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
    DBG(("geometry tb=%ld,%ld,%ld,%ld mon=%ld,%ld,%ld,%ld shown=%ld,%ld,%ld,%ld t=%lu\r\n",
         r.left, r.top, r.right, r.bottom, g_mon.left, g_mon.top, g_mon.right, g_mon.bottom,
         g_shown.left, g_shown.top, g_shown.right, g_shown.bottom, GetTickCount()));
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

/* Show delay and a moving hand: the cursor touches the edge, then wanders a
   few pixels (resting hand) or more (moving the mouse left and right along the
   bottom) off it and back. None of that may restart the delay, or the bar only
   comes up once the mouse is held still. So the delay counts from the first
   touch for as long as the cursor stays over the (hidden) bar's area, and only
   leaving that area cancels it.
   When the delay is over, the bar comes up if the cursor is at the edge or
   within 1/8 of the bar's thickness of it (a resting hand); otherwise as soon
   as it touches the edge again. */
static int inBarArea(POINT pt)
{
    if (pt.x < g_mon.left || pt.x >= g_mon.right || pt.y < g_mon.top || pt.y >= g_mon.bottom)
        return 0;
    switch (g_edge) {
    case ABE_TOP:   return pt.y < g_shown.bottom;
    case ABE_LEFT:  return pt.x < g_shown.right;
    case ABE_RIGHT: return pt.x >= g_shown.left;
    default:        return pt.y >= g_shown.top;
    }
}
static int nearEdge(POINT pt)
{
    int band = (g_edge == ABE_LEFT || g_edge == ABE_RIGHT ? g_shown.right - g_shown.left
                                                           : g_shown.bottom - g_shown.top) / 8;
    if (band < 2) band = 2;
    band += cfgTriggerPx;
    if (pt.x < g_mon.left || pt.x >= g_mon.right || pt.y < g_mon.top || pt.y >= g_mon.bottom)
        return 0;
    switch (g_edge) {
    case ABE_TOP:   return pt.y < g_mon.top + band;
    case ABE_LEFT:  return pt.x < g_mon.left + band;
    case ABE_RIGHT: return pt.x >= g_mon.right - band;
    default:        return pt.y >= g_mon.bottom - band;
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

static int isFullscreenWnd(HWND f)
{
    RECT r;
    if (!f || classIs(f, kDesktopClasses) || !GetWindowRect(f, &r)) return 0;
    /* With auto-hide a maximized window covers the whole screen too, so it has
       to be told apart: it has a title bar, or borders hanging off the screen.
       A browser that was maximized before going full screen (YouTube) stays
       "zoomed", but has neither. */
    if (IsZoomed(f) && ((GetWindowLongW(f, GWL_STYLE) & WS_CAPTION) == WS_CAPTION ||
                        r.left < g_mon.left || r.top < g_mon.top)) return 0;
    return r.left <= g_mon.left && r.top <= g_mon.top &&
           r.right >= g_mon.right && r.bottom >= g_mon.bottom;
}
static int fgIsFullscreen(void)
{
    return isFullscreenWnd(GetForegroundWindow());
}

/* the top normal window (below "always on top" ones, popups of the taskbar
   and FastBar's own) is one that covers the screen like a full-screen one:
   Windows then treats the bar differently (it goes below that window, and
   Explorer may pull it away when a tray flyout opens) */
static int fullscreenBehind(void)
{
    HWND w;
    DWORD me = GetCurrentProcessId(), pid;
    for (w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w) || isCloaked(w)) continue;
        if (GetWindowLongW(w, GWL_EXSTYLE) & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) continue;
        GetWindowThreadProcessId(w, &pid);
        if (pid == me || classIs(w, kPopupClasses)) continue;
        return isFullscreenWnd(w);
    }
    return 0;
}

/* Windows keeps the real bar below a full-screen window: shown there, it could
   not be clicked */
static int blockedFull(void)
{
    return cfgBlockFull && fgIsFullscreen();
}

/* ===================================================================== */
/* animation thread                                                       */
/* ===================================================================== */
/*
 * How the bar is put on screen
 * ----------------------------
 * The real taskbar never moves. Two facts about Explorer (Windows 11) force this:
 *  - moving the real bar while the cursor is over it makes Explorer process hover
 *    on every frame, stalling our SetWindowPos calls (~30 ms per frame);
 *  - in auto-hide mode Explorer only draws the bar's content once the real cursor
 *    has been over it (~80 ms later); it cannot be faked with window messages.
 * So the real bar stands still at its full position, and every slide (in, out,
 * and a slide that is turned around half-way) is played by a window of ours on
 * top of it, the cover: click-through and see-through, it shows nothing but a
 * live DWM thumbnail of the real bar, drawn at the slide's current offset.
 * Meanwhile the real bar is invisible (alpha 1, so it keeps rendering for the
 * thumbnail and still takes the clicks). No picture of the bar is ever taken:
 * what slides is the taskbar itself, with its current content, over whatever
 * is behind it right now.
 *
 *  C_REAL: the real bar, opaque, nothing of ours on screen.
 *  C_COMP: the cover with the live thumbnail; the real bar is invisible under it.
 *  a_prep: C_REAL, but the cover is already up, fully transparent, and the bar
 *          is layered: the two then swap within one frame, by their alpha (two
 *          calls of the same kind land in the same frame; a change of the
 *          thumbnail next to a change of alpha may not).
 */
static HWND    a_cover;         /* full bar area, see-through: holds the live thumbnail */
static int     a_carrier = C_NONE;
static double  a_p = 0.0;       /* 0 = hidden, 1 = shown */
static DWORD   a_explorerHideUntil;
static void keepOverlayOnTop(void);

/* window properties of the cover for the tests, which cannot see a thumbnail
   otherwise: the cover is showing the bar (it never hides what is under it),
   and how far (pixels) the thumbnail is from the bar's place */
#define SEE_THRU_PROP L"FastBarSeeThrough"
#define SLIDE_PROP    L"FastBarSlide"
#define COVER_KEY RGB(255, 0, 255)
static void ovAlpha(BYTE a)
{
    SetLayeredWindowAttributes(a_cover, COVER_KEY, a, LWA_COLORKEY | LWA_ALPHA);
    SetPropW(a_cover, SEE_THRU_PROP, (HANDLE)(INT_PTR)(a != 0));
}

/* The shield: over the bar's place, invisible (alpha 1) but solid to the
   mouse. The cover is see-through to the mouse, so while Explorer has the real
   bar away (pulled off its place and below a window it takes for full screen,
   Riot Client, see guard) the cursor would be over that window: it takes that
   window's shape, and a click would go there (activating it closes the tray's
   hidden icons). Explorer does not answer for ~200 ms then, so the bar could
   not take the click anyway. */
#define SHIELD_MAX_MS 500
static HWND  a_shield;
static DWORD a_shieldSince;
static void shieldOn(void)
{
    if (!a_shield) return;
    SetWindowPos(a_shield, a_cover, g_shown.left, g_shown.top, g_shown.right - g_shown.left,
                 g_shown.bottom - g_shown.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    a_shieldSince = GetTickCount();
}
static void shieldOff(void)
{
    if (a_shield && IsWindowVisible(a_shield)) ShowWindow(a_shield, SW_HIDE);
}
/* never for long: the real bar must take the clicks again */
static void shieldCheck(void)
{
    if (a_shield && IsWindowVisible(a_shield) && GetTickCount() - a_shieldSince > SHIELD_MAX_MS) shieldOff();
}

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

static void waitFrame(void)
{
    if (!pDwmFlush || FAILED(pDwmFlush())) Sleep(4);
}

static void pumpMessages(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

static int     a_realUnder;      /* the real bar is shown (invisible) under the cover */
static int     a_prep;           /* see above */
static int     a_hidePending;    /* the bar is off screen but could not be hidden yet (hideReal) */
static int     a_left;           /* the cursor has been away since the bar came up */
static DWORD   a_thumbFrom;      /* the thumbnail has been showing since this time */
static DWORD   a_revealAt;       /* earliest time the cover may be removed */
#define REVEAL_MS 40             /* a few frames for Explorer to settle */

/* In auto-hide mode Explorer hides the taskbar's content by hiding this child
   window, and only shows it again ~40-90 ms after the cursor reaches the bar.
   Showing it ourselves makes the live content available within one frame. */
static void showContent(void)
{
    HWND br = FindWindowExW(g_tb, NULL, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL);
    if (br && !IsWindowVisible(br)) ShowWindow(br, SW_SHOWNA);
}
static int contentShown(void)
{
    HWND br = FindWindowExW(g_tb, NULL, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL);
    return !br || IsWindowVisible(br);
}

/* Opacity of the real taskbar window. During a slide the real bar stands at
   its full position while the cover draws it somewhere else, so it is made
   transparent (it keeps rendering, so its live thumbnail still shows the
   content), and opaque again when the slide is done. No window of ours could
   hide it instead: while Start or Quick Settings is (or was) open, Windows can
   keep the taskbar in a z-band above every normal window.
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
/* The bar is made layered, at full opacity, ahead of a swap with the cover.
   Making a window layered waits for Explorer to handle the style change,
   which takes a few frames while it is busy. Already layered, only the alpha
   changes, which is immediate. */
static volatile LONG g_wantLayer;
/* D_HOLD: the bar waits to go and is pinned (1), or a flyout keeps it up, and
   only Explorer hiding it by itself ends that: not pinned (0) */
static volatile LONG g_holdPinned;
static volatile LONG g_tapPinned = -1;   /* what FastBarTap.dll was told last (tapPin); -1: not yet */
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

/* Windows keeps the real bar below a window it takes for full screen (Riot
   Client, a browser). Start lifts it, but when a tray flyout replaces Start the
   bar is back below that window. */
static int barCovered(void)
{
    POINT c;
    HWND w;
    c.x = (g_shown.left + g_shown.right) / 2;
    c.y = (g_shown.top + g_shown.bottom) / 2;
    w = GetAncestor(WindowFromPoint(c), GA_ROOT);
    return w && w != g_tb && !(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
           !classIs(w, kPopupClasses);
}
static void barRaise(void)
{
    if (barCovered())
        SetWindowPos(g_tb, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
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

/* Explorer keeps its own "hidden / shown" auto-hide state. If the bar is only
   put on screen while Explorer thinks it is hidden, the first click on it makes
   Explorer restart its own unhide (the bar jumps away and crawls back up).
   A WM_ACTIVATE message (not a real activation: focus stays where it is) makes
   Explorer switch to "shown" at once and draw its content. */
static void syncExplorerShown(void)
{
    /* Explorer takes the foreground back on this message, which closes the
       tray's overflow window (hidden icons). Its closing changes the foreground
       again, and that syncs the bar (TIMER_EXPLSYNC). */
    static const WCHAR *const overflow[] = {
        L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow", NULL
    };
    if (classIs(GetForegroundWindow(), overflow)) return;
    /* posted, not sent: Explorer can take a while to handle it, and the
       animation must not wait for that */
    PostMessageW(g_tb, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(g_tb, WM_ACTIVATE, WA_INACTIVE, 0);
}

/* show the real bar, invisible, under the cover */
static void realUnder(void)
{
    if (!a_realUnder) {
        tbAlpha(TB_INVISIBLE);             /* invisible until the cover goes */
        showRealFull();
        syncExplorerShown();
        keepOverlayOnTop();
        a_showStart = GetTickCount();
        a_revealAt = a_showStart + REVEAL_MS;
    }
    a_realUnder = 1;
}

/* may the cover go away now? */
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

/* the real bar under the cover stays invisible and keeps its content, whatever
   Explorer does to it meanwhile (its own auto-hide can run under the cover:
   it may drop the layered style, and it hides the content when it is done) */
static void keepUnder(void)
{
    if (!a_realUnder || !g_tb) return;
    if (!(GetWindowLongW(g_tb, GWL_EXSTYLE) & WS_EX_LAYERED)) {
        DBG(("keepUnder: layered style was dropped t=%lu\r\n", GetTickCount()));
        tbAlpha(TB_INVISIBLE);
    }
    showContent();
    /* Explorer can still be busy with a hide of its own when the bar comes
       (back) up, and go on moving it: off its place it would not take the
       clicks. Pinned, it stays where it is put. */
    if (g_desired != D_HIDDEN && g_tapPinned == 1 && !realAtFull()) {
        DBG(("keepUnder: bar put back t=%lu\r\n", GetTickCount()));
        showRealFull();
    }
}

/* ---------- the cover and its live thumbnail ---------- */
static HANDLE  a_thumb;
static HWND    a_thumbOf;

static void thumbOff(void)
{
    if (a_thumb && pDwmUnregThumb) pDwmUnregThumb(a_thumb);
    a_thumb = NULL;
}

/* the thumbnail at progress a_p */
static void thumbSet(void)
{
    THUMBPROPS tp;
    int x, y;
    if (!a_thumb || !pDwmUpdThumb) return;
    posAt(a_p, &x, &y);
    ZeroMemory(&tp, sizeof tp);
    tp.dwFlags = TNP_RECTDESTINATION | TNP_OPACITY | TNP_VISIBLE | TNP_CLIENTONLY;
    tp.rcDestination.left = x - g_shown.left;
    tp.rcDestination.top = y - g_shown.top;
    tp.rcDestination.right = tp.rcDestination.left + (g_shown.right - g_shown.left);
    tp.rcDestination.bottom = tp.rcDestination.top + (g_shown.bottom - g_shown.top);
    tp.opacity = 255;
    tp.fVisible = TRUE;
    tp.fSourceClientAreaOnly = FALSE;
    pDwmUpdThumb(a_thumb, &tp);
    SetPropW(a_cover, SLIDE_PROP, (HANDLE)(INT_PTR)((x - g_shown.left) + (y - g_shown.top)));
}

/* Put the cover on screen, its thumbnail at progress a_p. A window that is
   shown again first appears with what it showed last time, for one frame; so
   it is shown fully transparent, painted, and only then made opaque (showing),
   or left transparent for now (see a_prep). */
static int coverUp(int showing)
{
    RECT r;
    if (a_thumb && a_thumbOf != g_tb) thumbOff();
    if (!a_thumb) {
        if (!pDwmRegThumb || FAILED(pDwmRegThumb(a_cover, g_tb, &a_thumb))) {
            a_thumb = NULL;
            return 0;
        }
        a_thumbOf = g_tb;
    }
    thumbSet();
    if (!IsWindowVisible(a_cover) || !GetWindowRect(a_cover, &r) || !EqualRect(&r, &g_shown)) {
        ovAlpha(0);
        SetWindowPos(a_cover, HWND_TOPMOST, g_shown.left, g_shown.top, g_shown.right - g_shown.left,
                     g_shown.bottom - g_shown.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RedrawWindow(a_cover, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        waitFrame();
    }
    if (showing) ovAlpha(255);
    return 1;
}

static void coverDown(void)
{
    shieldOff();
    if (IsWindowVisible(a_cover)) ShowWindow(a_cover, SW_HIDE);
    thumbOff();
    a_prep = 0;
}

/* The cursor left the real bar (or a flyout is open): get ready to take the
   bar over. Nothing changes on screen. */
static int coverPrep(void)
{
    tbPrelayer();
    if (!coverUp(0)) return 0;
    a_prep = 1;
    return 1;
}

/* Explorer raises the real bar to the top of the "always on top" windows
   once the cursor is over it (and when a Quick Settings / calendar panel is
   open), which would put it above the cover in the middle of a slide. That
   only matters for the bar's clicks; keep the cover on top all the same. */
static void keepOverlayOnTop(void)
{
    if (a_carrier == C_COMP && IsWindowVisible(a_cover) && GetWindow(a_cover, GW_HWNDPREV) != NULL)
        SetWindowPos(a_cover, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER);
}

static int a_explSeen;           /* Explorer's own hide already reported */

/* The real bar (shown, opaque) -> the cover with its live thumbnail at the
   same spot, within one frame. The bar's see-through parts (the Search box, a
   see-through background) drawn by both would flash brighter; drawn by
   neither, the bar would blink.
   urgent: Explorer is already moving the bar by itself, which must not show. */
static int realToCover(int urgent)
{
    if (!a_prep) {
        if (urgent) tbAlpha(TB_INVISIBLE);
        else tbPrelayer();
        if (!coverUp(0)) {
            DBG(("realToCover: no cover t=%lu\r\n", GetTickCount()));
            if (urgent) tbAlpha(255);
            return 0;
        }
    }
    /* right after a frame: both land in the next one. (Urgent, the bar is
       already out of place: no time to lose.) */
    if (!urgent) waitFrame();
    a_thumbFrom = GetTickCount();
    thumbSet();
    ovAlpha(255);                      /* first: if timing slips, the bar is drawn twice
                                          for a moment rather than missing for a moment */
    tbAlpha(TB_INVISIBLE);
    a_prep = 0;
    a_realUnder = 1;
    a_explSeen = 0;
    a_carrier = C_COMP;
    keepOverlayOnTop();
    DBG(("realToCover urgent=%d t=%lu\r\n", urgent, GetTickCount()));
    return 1;
}

/* The cover -> the real bar, within one frame (see realToCover). The bar is
   still layered here, so its alpha changes at once; its own style comes back
   afterwards, when nothing shows anymore. */
static void handToReal(void)
{
    DBG(("handToReal start t=%lu\r\n", GetTickCount()));
    waitFrame();                       /* right after a frame: all land in the next one */
    tbAlpha(255);                      /* first: see realToCover */
    ovAlpha(0);
    waitFrame();
    coverDown();
    a_realUnder = 0;
    a_left = 0;
    a_carrier = C_REAL;
    if (!g_wantLayer) {
        waitFrame();
        tbAlphaRestore();              /* (a taskbar left layered reacts more slowly) */
    }
    DBG(("handToReal done t=%lu\r\n", GetTickCount()));
}

/* start a slide-in from fully hidden */
static void startShow(void)
{
    DBG(("startShow t=%lu\r\n", GetTickCount()));
    a_hidePending = 0;
    a_left = 0;
    a_explSeen = 0;
    if (cfgShowAnimMs <= 0) {
        coverDown();
        tbAlphaRestore();
        showRealFull();
        a_realUnder = 0;
        a_p = 1.0;
        a_carrier = C_REAL;
        return;
    }
    /* The real bar can still be on screen here: after Explorer hid it by itself
       its own slow hide animation runs for a while. It must not show. */
    if (IsWindowVisible(g_tb)) tbAlpha(TB_INVISIBLE);
    thumbOff();
    a_thumbFrom = GetTickCount();
    a_carrier = C_COMP;
    if (!coverUp(1)) {
        /* no thumbnail possible: just show the real bar */
        tbAlphaRestore();
        showRealFull();
        a_p = 1.0;
        a_carrier = C_REAL;
        return;
    }
    a_prep = 0;
    realUnder();                       /* invisible under the cover from the first frame */
}

/* Explorer restarted its own unhide on a bar we consider shown (it jumped to
   its hidden spot): the cover shows the bar in its place until it is back */
static void maskNativeUnhide(void)
{
    if (!realToCover(1)) return;
    a_showStart = GetTickCount();
    a_revealAt = a_showStart + REVEAL_MS;
}

/* The pin (FastBarTap.dll, see tapPin) keeps the real bar in place while the
   cursor is away and during the slide out, and also refuses to hide it. */
#define TAP_CLASS   L"FastBarTapWnd"
#define TAP_MSG_SET (WM_APP + 1)
#define TAP_MSG_PIN (WM_APP + 2)
static void tapUnpinNow(void)
{
    HWND w;
    DWORD_PTR res;
    int ok = 0;
    for (w = FindWindowExW(HWND_MESSAGE, NULL, TAP_CLASS, NULL); w;
         w = FindWindowExW(HWND_MESSAGE, w, TAP_CLASS, NULL))
        ok |= SendMessageTimeoutW(w, TAP_MSG_PIN, 0, 0, SMTO_ABORTIFHUNG, 60, &res) != 0;
    if (ok) InterlockedExchange(&g_tapPinned, 0);
}

/* The real bar goes off screen. Explorer can be busy for a while (in the
   middle of a hide of its own): then the pin is still on and refuses, and the
   bar stays invisible until guard() gets it hidden (a_hidePending). */
static int hideReal(void)
{
    if (!g_tb || !IsWindowVisible(g_tb)) return 1;
    tapUnpinNow();                     /* (whatever g_tapPinned says: the main thread
                                          may have posted a pin that is still on its way) */
    ShowWindow(g_tb, SW_HIDE);
    return !IsWindowVisible(g_tb);
}

/* the slide out is over (or there is none) */
static void finishHide(void)
{
    int i, gone = 0;
    coverDown();
    /* (the cursor is back meanwhile: the bar comes up again right away) */
    for (i = 0; i < 4 && g_desired == D_HIDDEN && !(gone = hideReal()); i++) waitFrame();
    if (gone) tbAlphaRestore();
    else tbAlpha(TB_INVISIBLE);
    a_hidePending = !gone;
    a_realUnder = 0;
    a_p = 0.0;
    a_carrier = C_NONE;
    DBG(("finishHide gone=%d t=%lu\r\n", gone, GetTickCount()));
}

/* a window went full screen over the shown bar: no slide */
static volatile LONG g_hideNow;
/* new geometry while the bar is up: put the real bar at its new spot (guard) */
static volatile LONG g_replace;

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
            if (InterlockedExchange(&g_hideNow, 0) && a_carrier != C_NONE) finishHide();
            if (a_carrier != C_NONE) a_left = 1;
            /* the real bar is taken over by the cover, which then slides */
            if (a_carrier == C_REAL && (cfgHideAnimMs <= 0 || !realToCover(0))) finishHide();
        } else {
            target = 1.0;
            if (a_carrier == C_NONE) startShow();
            if (d == D_HOLD) {
                /* cursor left: the bar stays as it is, live; a slide-in just
                   finishes. Pinned (it waits to go, see tapPin) the real bar
                   stays and gets ready for the slide out. Not pinned (a flyout
                   keeps it up, or FastBarTap.dll is missing) Explorer will
                   hide it by itself at some point, and the first step of that
                   would show: the cover takes over now, the pin goes, and
                   Explorer then moves an invisible bar (guard). */
                a_left = 1;
                if (a_carrier == C_REAL) {
                    DBG(("hold: real bar pin=%ld/%ld full=%d prep=%d t=%lu\r\n", g_holdPinned, g_tapPinned,
                         realAtFull(), a_prep, GetTickCount()));
                    if (g_holdPinned && g_tapPinned == 1) { if (!a_prep) coverPrep(); }
                    else if (realAtFull()) realToCover(0);
                }
                if (!g_holdPinned && g_tapPinned == 1 && g_desired == D_HOLD) tapUnpinNow();
            } else {
                if (a_left) {
                    /* the cursor is back. Explorer may have tried to hide the
                       bar meanwhile, and take it for hidden. */
                    a_left = 0;
                    if (a_carrier == C_COMP || !realAtFull() || !contentShown()) showRealFull();
                    DBG(("back: carrier=%d full=%d content=%d explSeen=%d t=%lu\r\n", a_carrier, realAtFull(),
                         contentShown(), a_explSeen, GetTickCount()));
                    /* (with a flyout open, only if Explorer did: the message
                       is not needed otherwise) */
                    if (!g_wantLayer || a_explSeen) syncExplorerShown();
                    a_showStart = GetTickCount();
                    a_revealAt = a_showStart + REVEAL_MS;
                }
                if (a_carrier == C_REAL && a_prep && !g_wantLayer) {
                    coverDown();
                    tbAlphaRestore();
                }
            }
        }

        if (a_p == target) {
            if (target == 0.0 && a_carrier != C_NONE) {
                if (g_desired != d) continue;      /* turned around at the last moment */
                finishHide();
            } else if (target == 1.0 && d == D_SHOWN && a_carrier == C_COMP) {
                if (realReady()) {
                    /* real bar is ready: opaque again, and its original window
                       style back. (The shield first: barRaise must see what
                       is really over the bar's place.) */
                    shieldOff();
                    barRaise();
                    handToReal();
                } else {
                    shieldCheck();
                    thumbSet();
                    keepUnder();
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

        if (a_carrier == C_COMP) {
            thumbSet();
            keepUnder();
        }
        keepOverlayOnTop();
        waitFrame();
        pumpMessages();
    }
}

/* keep things consistent if Explorer moves/shows the taskbar on its own */
static void guard(void)
{
    LONG d = g_desired;
    RECT r;
    if (g_paused || !g_tb) return;
    if (a_carrier == C_NONE && d == D_HIDDEN) {
        /* let Explorer finish its own hide animation before hiding the window */
        if (a_hidePending) {
            if (hideReal()) {
                DBG(("guard: bar hidden at last t=%lu\r\n", GetTickCount()));
                tbAlphaRestore();
                a_hidePending = 0;
            }
        } else if (IsWindowVisible(g_tb) && (LONG)(GetTickCount() - a_explorerHideUntil) >= 0) {
            ShowWindow(g_tb, SW_HIDE);
        }
    } else if (a_carrier == C_COMP && a_realUnder && d != D_HIDDEN) {
        /* the cover is up with the cursor away (a slide-in that finished after
           the cursor left, or a bar taken over below). Explorer hides the
           (invisible) real bar by itself: with a flyout open that is what ends
           the wait, so tell the main thread, which then hides the bar the
           normal FastBar way. (Not while it waits to go or a menu keeps it
           up, g_holdPinned: without FastBarTap.dll nothing stops Explorer
           then.) The bar stays in place for its clicks. */
        keepUnder();
        if (IsWindowVisible(g_tb) && GetWindowRect(g_tb, &r) &&
            (r.left - g_shown.left) * g_offX + (r.top - g_shown.top) * g_offY > 0) {
            if (d == D_HOLD && !a_explSeen && !g_holdPinned) {
                a_explSeen = 1;
                DBG(("guard: explorer hides the bar under the cover t=%lu\r\n", GetTickCount()));
                PostMessageW(g_hwnd, WM_APP_EXPLHID, 1, 0);
            }
            if (d == D_HOLD) placeReal(1.0);
        }
    } else if (a_carrier == C_REAL && a_p >= 1.0 && d != D_HIDDEN) {
        if (InterlockedExchange(&g_replace, 0) && IsWindowVisible(g_tb)) {
            placeReal(1.0);
            showContent();
        }
        if (realAtFull()) barRaise();
        /* cursor away (D_HOLD) or a flyout open: ready for a quick takeover */
        if (g_wantLayer || d == D_HOLD) {
            if (realAtFull()) coverPrep();
        } else {
            if (a_prep) coverDown();
            tbAlphaRestore();
        }
        if (!IsWindowVisible(g_tb)) showRealFull();
        else if (GetWindowRect(g_tb, &r) && (r.left != g_shown.left || r.top != g_shown.top)) {
            long dx = r.left - g_shown.left, dy = r.top - g_shown.top;
            long along = dx * g_offX + dy * g_offY;          /* > 0: towards hidden */
            long full = g_offX * g_offX + g_offY * g_offY;
            POINT pt;
            DBG(("guard moved dx=%ld dy=%ld along=%ld full=%ld d=%ld prep=%d t=%lu\r\n", dx, dy, along, full,
                 d, a_prep, GetTickCount()));
            GetCursorPos(&pt);
            if (along > 0 && g_wantLayer && cursorOnBar(pt) && fullscreenBehind()) {
                /* A tray flyout opened from Start over a window Windows takes
                   for full screen (Riot Client): Explorer pulls the bar to its
                   hidden spot under the cursor, past the pin, and does not
                   bring it back. Put it back at once. */
                DBG(("guard: bar pulled away with a flyout open, put back t=%lu\r\n", GetTickCount()));
                /* It also drops the bar below that window, and stays busy for
                   ~250 ms: putting the bar back and raising it wait for
                   Explorer, and meanwhile the bar would be gone (a see-through
                   bar seems to lose its icons). The cover shows it in place
                   from the next frame on, until the real one is back on top
                   (animate: realReady, barRaise, handToReal). */
                maskNativeUnhide();
                if (a_carrier == C_COMP) shieldOn();   /* before anything that waits for Explorer */
                placeReal(1.0);
                showContent();
                SetEvent(g_evt);
            } else if (d == D_SHOWN && (along * 4 >= full * 3 || (along > 0 && cursorOnBar(pt)))) {
                /* jumped (almost) all the way: Explorer restarts its own unhide.
                   Or Explorer started its own hide just as the cursor came back
                   onto the bar: it brings the bar back by itself. Either way,
                   the cover shows it in place until it is back. */
                maskNativeUnhide();
                SetEvent(g_evt);
            } else if (along > 0 && realToCover(1)) {
                /* Explorer started hiding the bar by itself (the cursor is away;
                   e.g. it went into Quick Settings / the calendar): the cover
                   takes the bar over where it stood, live, the real one goes
                   back to its place under it, and the bar is then hidden the
                   normal FastBar way */
                placeReal(1.0);
                showContent();
                a_p = 1.0;
                a_explSeen = 1;
                a_explorerHideUntil = GetTickCount() + 600;
                InterlockedExchange(&g_desired, D_HOLD);
                PostMessageW(g_hwnd, WM_APP_EXPLHID, 1, 0);
            } else if (along > 0) {
                /* no cover possible: go along with Explorer's own hide */
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

static LRESULT CALLBACK CoverProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        /* all of it is the color key: only the live thumbnail shows, drawn
           on top by DWM */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HBRUSH b = CreateSolidBrush(COVER_KEY);
        FillRect(dc, &ps.rcPaint, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, m, w, l);
}

/* see shieldOn: takes the mouse and does nothing with it */
static LRESULT CALLBACK ShieldProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATEANDEAT;
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI AnimThread(LPVOID unused)
{
    WNDCLASSW wc = { 0 };
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    wc.lpfnWndProc = CoverProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"FastBarOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    /* click-through (layered + transparent): the mouse goes to the real bar
       underneath, which is what makes Explorer draw it */
    a_cover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                              WS_EX_LAYERED | WS_EX_TRANSPARENT,
                              wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
                              NULL, NULL, wc.hInstance, NULL);
    ovAlpha(255);
    wc.lpfnWndProc = ShieldProc;
    wc.lpszClassName = L"FastBarShield";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    a_shield = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                               wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (a_shield) SetLayeredWindowAttributes(a_shield, 0, 1, LWA_ALPHA);
    for (;;) {
        int shield = a_shield && IsWindowVisible(a_shield);
        DWORD r = MsgWaitForMultipleObjects(1, &g_evt, FALSE, shield ? 50 : a_hidePending ? 30 : INFINITE,
                                            QS_ALLINPUT);
        pumpMessages();
        if (shield) shieldCheck();
        if (r == WAIT_OBJECT_0 || (r == WAIT_TIMEOUT && a_hidePending)) {
            InterlockedExchange(&g_animIdle, 0);
            animate();
            guard();
            InterlockedExchange(&g_animIdle, 1);
        }
    }
    return 0;
}

static void setDesired(LONG d)
{
    DBG(("setDesired %ld mode=%d t=%lu\r\n", d, g_mode, GetTickCount()));
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
    if (a_cover) ShowWindow(a_cover, SW_HIDE);
    thumbOff();
    a_prep = 0;
    a_hidePending = 0;
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
    if (under && under != a_cover) {
        HWND root = GetAncestor(under, GA_ROOT);
        static const WCHAR *const cw[] = { L"Windows.UI.Core.CoreWindow", NULL };
        if (root != a_cover && classIs(root, kPopupClasses) && !isCloaked(root) &&
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
   keeps the bar up (D_HOLD while in M_SHOWN; the name is from when the bar was
   frozen into a picture for this: it stays live now) */
static int g_frozen;
/* Win+T / Win+B: the keyboard focus went to the hidden bar (the foreground
   switched to it while it was hidden). Explorer would unhide it; FastBar shows
   it and keeps it live and up for as long as the focus stays there. */
static int g_kbFocus;
static const WCHAR *const kTrayClasses[] = { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", NULL };

/* should the bar stay up / come back? */
/* A menu opened from the bar is still open: the bar's own (right-click on the
   bar or on Start) or a tray icon's. The foreground stays on the bar for these,
   so fgKeeps() does not see them. Only menus next to the bar count.
   The bar's own menu is a XAML popup that opens where the bar was right-clicked
   (mouse hook). Its tooltips are XAML popups with the same styles (one stays up
   while a bar button has the keyboard focus), so only that popup counts. */
static int   g_rclickArm;
static POINT g_rclickPt;
static DWORD g_rclickAt;
static HWND  g_menuWnd;
static HWND  g_preMenu[8];               /* XAML popups already up at the click */
static int   g_preMenuN;

static void armBarMenu(POINT pt)
{
    HWND h = NULL;
    g_rclickArm = 1;
    g_rclickPt = pt;
    g_rclickAt = GetTickCount();
    g_menuWnd = NULL;
    /* a tooltip (the clicked button's, shown on hover) is up before the click */
    g_preMenuN = 0;
    while ((h = FindWindowExW(NULL, h, L"Xaml_WindowedPopupClass", NULL)) != NULL && g_preMenuN < 8)
        if (IsWindowVisible(h)) g_preMenu[g_preMenuN++] = h;
}

static int barMenuOpen(void)
{
    RECT nearBar = g_shown, r, x;
    HWND h = NULL;
    InflateRect(&nearBar, 24, 24);
    /* a tray icon's menu (its own program's): this class is always a menu */
    while ((h = FindWindowExW(NULL, h, L"#32768", NULL)) != NULL)
        if (IsWindowVisible(h) && !isCloaked(h) && GetWindowRect(h, &r) &&
            r.right - r.left > 20 && r.bottom - r.top > 8 && IntersectRect(&x, &r, &nearBar)) {
            g_rclickArm = 0;               /* the right-click was on a tray icon */
            g_menuWnd = NULL;
            return 1;
        }
    if (g_menuWnd) {
        if (IsWindow(g_menuWnd) && IsWindowVisible(g_menuWnd) && !isCloaked(g_menuWnd)) return 1;
        g_menuWnd = NULL;                  /* closed */
        g_rclickArm = 0;
        return 0;
    }
    if (!g_rclickArm) return 0;
    if (GetTickCount() - g_rclickAt > 1500) { g_rclickArm = 0; return 0; }   /* no menu came */
    h = NULL;
    while ((h = FindWindowExW(NULL, h, L"Xaml_WindowedPopupClass", NULL)) != NULL) {
        int i, old = 0;
        for (i = 0; i < g_preMenuN; i++) old |= g_preMenu[i] == h;
        if (!old && IsWindowVisible(h) && !isCloaked(h) && GetWindowRect(h, &r) &&
            r.left - 8 <= g_rclickPt.x && g_rclickPt.x <= r.right + 8 && IntersectRect(&x, &r, &nearBar)) {
            g_menuWnd = h;
            return 1;
        }
    }
    return 1;                              /* still opening */
}

static int wantShown(POINT pt, HWND fg)
{
    if (g_explHid) return cursorOnBar(pt);
    return cursorInside(pt) || fgKeeps(fg) || g_kbFocus || barMenuOpen();
}

static void killShow(void) { KillTimer(g_hwnd, TIMER_SHOW); g_showPending = 0; }

static void showNow(void)
{
    DBG(("showNow pending=%d t=%lu\r\n", g_showPending, GetTickCount()));
    killShow();
    KillTimer(g_hwnd, TIMER_HIDE);
    g_mode = M_SHOWN;
    g_frozen = 0;
    g_explHid = 0;
    tapPin(1);                      /* from the start: see keepUnder() */
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
    /* the cursor is on the shown bar: Explorer must not hide it (tap.c) */
    if (g_kbFocus && !classIs(fg, kTrayClasses)) g_kbFocus = 0;   /* focus went elsewhere */
    /* ... nor while the cursor is away and the bar waits to go, nor during
       the slide out: the real bar stays in place for its live thumbnail */
    if (g_mode != M_SHOWN) {
        tapPin(g_mode == M_LEAVING || a_carrier != C_NONE);
    } else if (cursorOnBar(pt) || g_kbFocus || !wantShown(pt, fg)) {
        tapPin(1);                  /* on the bar, or about to wait and go */
    } else if (cursorInside(pt)) {
        tapPin(0);                  /* on one of the bar's popups */
    } else if (!fgKeeps(fg)) {
        tapPin(1);                  /* cursor away, a menu keeps the bar up */
    }
    /* else the cursor is away and a flyout keeps the bar up, until Explorer
       hides it by itself (D_HOLD below). The anim thread lets the bar go, once
       the cover has it: a move Explorer still had in store lands the moment
       the pin goes, and must not show. */
    DBG(("eval pt=%ld,%ld mode=%d mon=%ld,%ld,%ld,%ld edge=%d at=%d full=%d t=%lu\r\n", pt.x, pt.y, g_mode,
         g_mon.left, g_mon.top, g_mon.right, g_mon.bottom, g_edge, atEdge(pt), fgIsFullscreen(),
         GetTickCount()));

    if (g_mode == M_HIDDEN) {
        if (cfgShowOnStart && isStartOrSearch(fg)) { showNow(); return; }
        if (g_kbFocus && !blockedFull()) { showNow(); return; }
        if (atEdge(pt) && !blockedFull()) {
            if (cfgShowDelay <= 0) showNow();
            else if (g_showPending && (LONG)(GetTickCount() - g_showSince) >= cfgShowDelay)
                showNow();          /* delay over (the timer may also be late) */
            else if (!g_showPending) {
                /* While the cursor rests on the edge, Explorer's own auto-hide
                   would "unhide" the (hidden) bar by itself, and then also hide
                   it by itself when the cursor leaves: its 30 fps slide next to
                   ours (a second taskbar). Mark the bar "shown" for Explorer
                   right away, as a FastBar show does; the window itself stays
                   hidden until the delay ends. Explorer can still start its own
                   hide now and then; guard() takes the real bar away then. */
                syncExplorerShown();
                g_showPending = 1;
                g_showSince = GetTickCount();
                SetTimer(g_hwnd, TIMER_SHOW, cfgShowDelay, NULL);
                DBG(("show delay starts pt=%ld,%ld t=%lu\r\n", pt.x, pt.y, GetTickCount()));
            }
        } else if (!(g_showPending && inBarArea(pt) && !blockedFull())) {
            killShow();             /* left the bar's area: start over next time */
        } else if ((LONG)(GetTickCount() - g_showSince) >= cfgShowDelay && nearEdge(pt)) {
            showNow();              /* delay over, cursor back near the edge */
        }
        return;
    }

    if (blockedFull()) {
        /* went full screen while the bar was up (F11 / YouTube's "f" with the
           cursor on the bar, or back from Start): the bar is covered now */
        KillTimer(g_hwnd, TIMER_HIDE);
        g_mode = M_HIDDEN;
        g_explHid = 0;
        g_frozen = 0;
        g_onBar = 0;
        InterlockedExchange(&g_hideNow, 1);
        tapPin(0);
        setDesired(D_HIDDEN);
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
        int on = cursorOnBar(pt);
        if (g_desired == D_SHOWN && barCovered()) SetEvent(g_evt);   /* guard raises it */
        if (!on && g_onBar) g_leaveAt = GetTickCount();
        g_onBar = on;
    }
    {
        /* a flyout is open: get the bar ready for a quick takeover (coverPrep) */
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
            /* kept up only by an open flyout, cursor elsewhere (D_HOLD): the
               bar stays live, until Explorer hides it by itself (guard) */
            int frz = !cursorInside(pt) && !g_kbFocus;
            LONG hp = !fgKeeps(fg);                      /* a menu: the bar stays for sure */
            if (frz != g_frozen || (frz && hp != g_holdPinned)) {
                g_frozen = frz;
                InterlockedExchange(&g_holdPinned, hp);
                setDesired(frz ? D_HOLD : D_SHOWN);
            }
        }
    } else if (g_mode == M_SHOWN) {
        g_frozen = 0;
        g_mode = M_LEAVING;
        InterlockedExchange(&g_holdPinned, 1);
        tapPin(1);                  /* the bar stays in place until it has slid out */
        if (cfgHideDelay <= 0) { g_mode = M_HIDDEN; setDesired(D_HIDDEN); }
        else { setDesired(D_HOLD); SetTimer(g_hwnd, TIMER_HIDE, cfgHideDelay, NULL); }
    }
}

/* A mouse moves by relative steps, and the hook gets the new point before
   Windows stops the cursor at the screen's edge: pulling the mouse down onto
   the bottom edge (or pushing down while moving along it) reports points
   below the screen. Put the point where the cursor will really be, the
   nearest point on a monitor, or none of them counts as "at the edge". */
static POINT onScreen(POINT pt)
{
    MONITORINFO mi = { sizeof mi };
    if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) return pt;
    if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) return pt;
    if (pt.x < mi.rcMonitor.left) pt.x = mi.rcMonitor.left;
    if (pt.x >= mi.rcMonitor.right) pt.x = mi.rcMonitor.right - 1;
    if (pt.y < mi.rcMonitor.top) pt.y = mi.rcMonitor.top;
    if (pt.y >= mi.rcMonitor.bottom) pt.y = mi.rcMonitor.bottom - 1;
    return pt;
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_enabled) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        POINT pt = onScreen(m->pt);
        int relevant;
        g_hookPt = pt;
        g_hookFresh = 1;
        if (wp == WM_LBUTTONDOWN)
            g_dragInside = (g_mode != M_HIDDEN) && PtInRect(&g_shown, pt);
        if (wp == WM_RBUTTONUP && g_mode != M_HIDDEN && PtInRect(&g_shown, pt)) {
            armBarMenu(pt);                /* see barMenuOpen */
            /* no menu came (barMenuOpen gives up after 1.5 s): look again then,
               even if the cursor rests away from the bar */
            g_reevalLeft = 1;
            SetTimer(g_hwnd, TIMER_REEVAL, 1600, NULL);
        }
        /* while hidden only the screen edge matters: skip everything else */
        relevant = (g_mode != M_HIDDEN) || atEdge(pt) || cfgShowDelay > 0;
        if (relevant && !InterlockedExchange(&g_evalPosted, 1))
            PostMessageW(g_hwnd, WM_APP_EVAL, 0, 0);
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK hk, DWORD ev, HWND h, LONG obj, LONG child,
                                  DWORD thread, DWORD time)
{
    (void)hk; (void)child; (void)thread; (void)time;
    if (ev == EVENT_SYSTEM_MENUPOPUPEND || (ev == EVENT_OBJECT_HIDE && obj == OBJID_WINDOW && h != g_tb)) {
        /* a menu (or another popup of Explorer) closed: may the bar go now?
           (see barMenuOpen; the cursor may be resting away from the bar) */
        if (g_mode != M_HIDDEN) PostMessageW(g_hwnd, WM_APP_EVAL, 0, 0);
        /* the bar's content was hidden under the cover: guard shows it again */
        if (ev == EVENT_OBJECT_HIDE && !g_paused && g_animIdle && GetParent(h) == g_tb) SetEvent(g_evt);
        return;
    }
    if (ev == EVENT_SYSTEM_FOREGROUND) {
        /* Win+T / Win+B come from a program's window. A flyout or Start that
           closes hands the foreground back to the bar too (Esc on the
           calendar with the bar hidden): that is no keyboard focus. */
        static HWND lastFg;
        static const WCHAR *const passing[] = { L"ForegroundStaging", NULL };
        if (g_mode == M_HIDDEN && classIs(h, kTrayClasses) && lastFg &&
            !classIs(lastFg, kFgKeepClasses) && !classIs(lastFg, kTrayClasses) &&
            !classIs(lastFg, kPopupClasses))
            g_kbFocus = 1;
        if (h && !classIs(h, passing)) lastFg = h;
        /* Explorer tried to hide the pinned bar and now believes it is hidden;
           the first click would restart its own unhide. Told it is shown once
           that is over, as a FastBar show does. */
        if (g_mode == M_SHOWN) SetTimer(g_hwnd, TIMER_EXPLSYNC, 700, NULL);
        PostMessageW(g_hwnd, WM_APP_EVAL, 0, 0);
        /* Start/search close with an animation; look again once it is gone */
        g_reevalLeft = 3;
        SetTimer(g_hwnd, TIMER_REEVAL, 150, NULL);
    } else if (h == g_tb && obj == OBJID_WINDOW && !g_paused) {
        /* Explorer gave the bar a new size (resolution or scaling changed):
           FastBar's geometry was read before that, see geometryChanged() */
        RECT r;
        if (GetWindowRect(g_tb, &r) && (r.right - r.left != g_shown.right - g_shown.left ||
                                        r.bottom - r.top != g_shown.bottom - g_shown.top))
            PostMessageW(g_hwnd, WM_APP_GEOM, 0, 0);
        if (g_animIdle) SetEvent(g_evt);   /* let the anim thread re-check (guard) */
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
    g_showHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, NULL, WinEventProc,
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

/* ---------- see-through taskbar background ----------
   FastBarTap.dll (64-bit) is loaded into Explorer by 64-bit rundll32 and
   makes the taskbar's XAML background clear (see tap.c). Once it is there,
   it is switched with messages to its windows. */
static HANDLE g_tapProc;            /* rundll32 loading the DLL */

static void tapPin(int on)
{
    HWND w;
    int n = 0;
    if (on == g_tapPinned) return;
#ifdef FB_NOPIN                      /* tests: as if FastBarTap.dll were missing */
    return;
#endif
    for (w = FindWindowExW(HWND_MESSAGE, NULL, TAP_CLASS, NULL); w;
         w = FindWindowExW(HWND_MESSAGE, w, TAP_CLASS, NULL), n++)
        PostMessageW(w, TAP_MSG_PIN, on, 0);
    DBG(("tapPin %d windows=%d t=%lu\r\n", on, n, GetTickCount()));
    if (n) InterlockedExchange(&g_tapPinned, on);   /* DLL not there (yet): try again next time */
}

static int tapSend(int on)
{
    HWND w;
    int n = 0;
    for (w = FindWindowExW(HWND_MESSAGE, NULL, TAP_CLASS, NULL); w;
         w = FindWindowExW(HWND_MESSAGE, w, TAP_CLASS, NULL), n++)
        PostMessageW(w, TAP_MSG_SET, on, (LPARAM)(LONG)cfgColor);
    return n;
}

static void tapApply(void)
{
    WCHAR dll[MAX_PATH], line[2 * MAX_PATH + 64], *slash, *p;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    BOOL wow = FALSE;
    typedef BOOL (WINAPI *PFN_IsWow64)(HANDLE, PBOOL);
    PFN_IsWow64 isWow;
    UINT n;
    /* loaded in any case: the pin is needed with a solid taskbar too */
    if (tapSend(cfgTransparent)) return;     /* already in Explorer */
    if (g_tapProc) {
        if (WaitForSingleObject(g_tapProc, 0) == WAIT_TIMEOUT) return;   /* still loading */
        CloseHandle(g_tapProc);
        g_tapProc = NULL;
    }
    if (!FindWindowW(L"Shell_TrayWnd", NULL)) return;
    lstrcpyW(dll, g_exePath);
    for (slash = p = dll; *p; p++) if (*p == L'\\') slash = p + 1;
    lstrcpyW(slash, L"FastBarTap.dll");
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) return;
    isWow = (PFN_IsWow64)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    if (isWow) isWow(GetCurrentProcess(), &wow);
    /* the DLL is 64-bit: from a 32-bit FastBar, System32 is redirected */
    n = GetWindowsDirectoryW(line + 1, MAX_PATH);
    line[0] = L'"';
    {
        WCHAR arg[16];
        if (cfgTransparent) wsprintfW(arg, L"%08lX", cfgColor);
        else lstrcpyW(arg, L"off");
        wsprintfW(line + 1 + n, L"%s\" \"%s\",Inject %s",
                  wow ? L"\\Sysnative\\rundll32.exe" : L"\\System32\\rundll32.exe", dll, arg);
    }
    if (CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        g_tapProc = pi.hProcess;
    }
}

static void setTransparent(int on)
{
    InterlockedExchange(&cfgTransparent, on);
    iniSetInt(L"TransparentTaskbar", on);
    tapApply();
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

/* "Automatically hide the taskbar" was switched in Settings while we run.
   Off: the user wants a normal taskbar, so step aside (otherwise the bar stays
   hidden by us and its reserved space shows as an empty strip). On: take over
   again. Either way it becomes the setting restored on exit. */
static void autoHideChangedByUser(void)
{
    int cur;
    if (g_settingAutoHide || g_lastAutoHide < 0) return;
    cur = getAutoHide();
    if (cur == g_lastAutoHide) return;
    g_lastAutoHide = cur;
    g_origAutoHide = cur;
    iniSetInt(L"OriginalAutoHide", cur);
    if (!cur && g_enabled) setEnabled(0);
    else if (cur && !g_enabled) setEnabled(1);
}

/* ---------- tray icon ---------- */
/* the logo is embedded as icon resource 1 (fastbar.rc -> assets/FastBar.ico) */
static HICON makeIcon(void)
{
    return (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
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
    AppendMenuW(m, MF_STRING | (cfgTransparent ? MF_CHECKED : 0), ID_TRANSPARENT, L"Transparent taskbar");
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
    case ID_TRANSPARENT: setTransparent(!cfgTransparent); break;
    case ID_RELOAD:   loadSettings(); tapApply(); break;
    case ID_EXIT:     DestroyWindow(g_hwnd); break;
    }
}

/* The bar's size or the screen changed: new geometry, and a bar that is up is
   put where it now belongs (the anim thread does it, see g_replace) */
static int g_geomLeft;
static void geometryChanged(void)
{
    RECT old = g_shown, oldMon = g_mon;
    updateGeometry();
    if (EqualRect(&old, &g_shown) && EqualRect(&oldMon, &g_mon)) return;
    DBG(("geometry changed, mode=%d t=%lu\r\n", g_mode, GetTickCount()));
    if (g_mode != M_HIDDEN) {
        InterlockedExchange(&g_replace, 1);
        setDesired(g_desired);
    }
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_APP_EXPLHID:          /* Explorer hid the bar on its own */
        DBG(("explhid w=%d mode=%d t=%lu\r\n", (int)w, g_mode, GetTickCount()));
        if (w) {
            /* the cover shows the bar (cursor away with a flyout open, or taken over by
               guard): hide it the normal way, the hide delay counted from
               when the cursor left the bar. Already leaving: nothing to do. */
            LONG rest = cfgHideDelay - (LONG)(GetTickCount() - g_leaveAt);
            POINT pt;
            if (g_mode != M_SHOWN) return 0;
            /* posted by the anim thread a moment ago: if the cursor has come
               back onto the bar / the edge since, the show it started wins.
               (Taking over here hid the bar with the cursor resting on the
               edge, and with no further mouse event it never came back.) */
            GetCursorPos(&pt);
            if (cursorOnBar(pt)) {
                /* the anim thread may have switched to "hold" itself: undo */
                if (g_desired != D_SHOWN) setDesired(D_SHOWN);
                return 0;
            }
            killShow();
            g_explHid = 1;
            g_frozen = 0;
            g_onBar = 0;
            DBG(("explhid takeover rest=%ld\r\n", rest));
            InterlockedExchange(&g_holdPinned, 1);
            tapPin(1);              /* Explorer leaves the bar alone from here on */
            if (rest <= 0) { g_mode = M_HIDDEN; g_explHid = 0; setDesired(D_HIDDEN); }
            else { g_mode = M_LEAVING; setDesired(D_HOLD); SetTimer(h, TIMER_HIDE, (UINT)rest, NULL); }
            return 0;
        }
        killShow();
        KillTimer(h, TIMER_HIDE);
        g_explHid = 0;
        g_mode = M_HIDDEN;
        SetTimer(h, TIMER_EXPLHID, 650, NULL);  /* then hide the window for real */
        PostMessageW(h, WM_APP_EVAL, 0, 0);     /* cursor already resting on the edge? */
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
        if (w == TIMER_TAP) {
            tapApply();
            return 0;
        }
        if (w == TIMER_GEOM) {
            if (g_enabled && g_tb) geometryChanged();
            if (--g_geomLeft > 0) SetTimer(h, TIMER_GEOM, 300, NULL);
            return 0;
        }
        if (w == TIMER_EXPLSYNC) {
            POINT pt;
            GetCursorPos(&pt);
            if (g_mode == M_SHOWN && cursorOnBar(pt)) syncExplorerShown();
            return 0;
        }
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
            /* delay over: up now if the cursor is (about) at the edge; else
               g_showPending stays set and evaluate() shows the bar as soon as
               the cursor touches the edge again (or forgets it once the cursor
               leaves the bar's area) */
            if (g_mode == M_HIDDEN && nearEdge(pt) && !blockedFull()) showNow();
            else if (g_mode != M_HIDDEN) g_showPending = 0;
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
    case WM_SETTINGCHANGE:
        autoHideChangedByUser();
        if (cfgTransparent && !g_settingAutoHide) SetTimer(h, TIMER_TAP, 800, NULL);
        /* fall through */
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        if (g_enabled && g_tb) {
            geometryChanged();
            /* Explorer resizes the bar for the new resolution / scaling only
               after this message: look again a few times */
            g_geomLeft = 6;
            SetTimer(h, TIMER_GEOM, 300, NULL);
        }
        return 0;
    case WM_APP_GEOM:
        if (g_enabled && g_tb) geometryChanged();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w) restoreTaskbar();
        return 0;
    case WM_DESTROY:
        if (g_enabled) restoreTaskbar();
        tapPin(0);
        tapSend(0);                         /* the taskbar's own background back */
        trayRemove();
        PostQuitMessage(0);
        return 0;
    }
    if (m == g_msgTaskbarCreated && m) {   /* Explorer restarted */
        trayAdd();
        g_tapPinned = -1;
        SetTimer(h, TIMER_TAP, 2000, NULL);    /* once its XAML is up */
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
    tapPin(0);
    tapSend(0);
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
    g_tapPinned = 1;                           /* (this process never told it) */
    tapPin(0);
    tapSend(0);                                /* see-through background off */
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

/* Keep "Start menu\Programs\FastBar.lnk" pointing at this exe, so FastBar can be
   found with Windows Search / the Start menu. Rewritten only when missing or
   when the exe was moved. */
static void ensureStartMenuShortcut(void)
{
    WCHAR lnk[MAX_PATH], cur[MAX_PATH], dir[MAX_PATH], *p, *slash;
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = 0;

    if (SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, lnk) != S_OK) return;
    if (lstrlenW(lnk) + 13 >= MAX_PATH) return;
    lstrcatW(lnk, L"\\FastBar.lnk");
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void **)&sl)) &&
        SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        cur[0] = 0;
        if (GetFileAttributesW(lnk) != INVALID_FILE_ATTRIBUTES &&
            SUCCEEDED(pf->lpVtbl->Load(pf, lnk, STGM_READ)) &&
            SUCCEEDED(sl->lpVtbl->GetPath(sl, cur, MAX_PATH, NULL, 0)))
            ok = lstrcmpiW(cur, g_exePath) == 0;
        if (!ok) {
            lstrcpyW(dir, g_exePath);
            slash = dir;
            for (p = dir; *p; p++) if (*p == L'\\') slash = p;
            *slash = 0;
            sl->lpVtbl->SetPath(sl, g_exePath);
            sl->lpVtbl->SetArguments(sl, L"");
            sl->lpVtbl->SetWorkingDirectory(sl, dir);
            sl->lpVtbl->SetIconLocation(sl, g_exePath, 0);
            sl->lpVtbl->SetDescription(sl, L"Responsive auto-hide taskbar");
            pf->lpVtbl->Save(pf, lnk, TRUE);
        }
    }
    if (pf) pf->lpVtbl->Release(pf);
    if (sl) sl->lpVtbl->Release(sl);
    CoUninitialize();
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
    ensureStartMenuShortcut();

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
    g_lastAutoHide = getAutoHide();

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hi;
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"FastBarMain";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hi, NULL);

    g_icon = makeIcon();
    trayAdd();

    g_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_animThread = CreateThread(NULL, 64 * 1024, AnimThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    while (!a_cover) Sleep(1);

    engage();
    startGuard();
    tapApply();
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEventProc,
                               0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    /* tray icons' menus belong to their own programs: their closing comes from here */
    g_menuHook = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPEND, EVENT_SYSTEM_MENUPOPUPEND, NULL, WinEventProc,
                                 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    /* drop startup pages: keeps the resident footprint tiny */
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_mouseHook);
    UnhookWinEvent(g_fgHook);
    if (g_menuHook) UnhookWinEvent(g_menuHook);
    unhookShell();
    ReleaseMutex(mtx);
    return 0;
}
