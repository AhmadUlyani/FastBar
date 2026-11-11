/*
 * record - plays a small mouse script against the running FastBar and records
 * the bottom of the screen (the bar's area and as much above it) on every DWM
 * frame, to see what was really on screen: a bar that blinks or is drawn
 * twice when FastBar swaps the real bar and its live thumbnail, a slide that
 * stalls, a second taskbar.
 *
 * usage: record.exe <outprefix> <step>...
 *   steps: e = glide to the bottom edge     b = jump onto the middle of the bar
 *          o = glide out to mid screen      <number> = wait that many ms
 *          q = Quick Settings (Win+A)       n = calendar (Win+N)     x = Esc
 *          w = Win (Start)
 *          c<x>,<y> / r<x>,<y> = glide there, left / right click
 *          F<title> = bring the window with that title to the front
 *   e.g.   record.exe tests\out\rec e 900 o 1200
 * writes <outprefix>.raw: frames of W x H BGRA pixels, and <outprefix>.txt:
 * a header "W H x0 y0 barTop", then per frame "<ms> tb=<y> v<visible> a<alpha>
 * cov=<visible> slide=<px> top=<bar on top at its middle> cur=<cursor>
 * ov=<tray's hidden icons open>" and "--<step>" marker lines, plus
 * "#cur <ms> <cursor> <class of the window under it>" whenever either changes
 * (looked at every ms) and "#busy <ms> <took>" when Explorer took more than
 * 30 ms to answer. tests\record.py turns the frames into numbers and pictures.
 * (F<title> may fail when Windows refuses to switch the foreground; Alt+Tab
 * to the window first then.)
 *
 * Each frame is taken about 5 ms after DwmFlush returns: right after it, a
 * capture can show a state between two of FastBar's calls that never reaches
 * the monitor.
 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *PFN_DwmFlush)(void);
typedef BOOL (WINAPI *PFN_GLWA)(HWND, COLORREF *, BYTE *, DWORD *);
static int SW, SH, g_barH;
static int g_argc;
static char **g_argv;
static volatile LONG g_done;
static FILE *g_txt;
static CRITICAL_SECTION g_cs;

static void moveTo(int x, int y)
{
    INPUT in = { 0 };
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((x * 65535.0) / (SW - 1));
    in.mi.dy = (LONG)((y * 65535.0) / (SH - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof in);
}
static void glide(int x0, int y0, int x1, int y1, int ms)
{
    int i, n = ms / 8;
    if (n < 1) n = 1;
    for (i = 1; i <= n; i++) { moveTo(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n); Sleep(8); }
}

static void keys(WORD a, WORD b)
{
    INPUT in[4];
    int i, n = 0;
    ZeroMemory(in, sizeof in);
    in[n].ki.wVk = a; n++;
    if (b) { in[n].ki.wVk = b; n++; in[n].ki.wVk = b; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    in[n].ki.wVk = a; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    for (i = 0; i < n; i++) in[i].type = INPUT_KEYBOARD;
    SendInput(n, in, sizeof in[0]);
}

static DWORD WINAPI script(LPVOID p)
{
    int i, x = SW / 5;                   /* away from the recorded part of the bar */
    POINT c;
    (void)p;
    for (i = 2; i < g_argc; i++) {
        const char *s = g_argv[i];
        GetCursorPos(&c);
        if (*s >= '0' && *s <= '9') { Sleep(atoi(s)); continue; }
        EnterCriticalSection(&g_cs);
        fprintf(g_txt, "--%s\n", s);
        LeaveCriticalSection(&g_cs);
        if (*s == 'e') glide(x, c.y, x, SH - 1, 40);
        else if (*s == 'b') moveTo(x, SH - g_barH / 2);
        else if (*s == 'o') glide(x, c.y, x, SH / 2, 40);
        else if (*s == 'q') keys(VK_LWIN, 'A');
        else if (*s == 'n') keys(VK_LWIN, 'N');
        else if (*s == 'x') keys(VK_ESCAPE, 0);
        else if (*s == 'w') keys(VK_LWIN, 0);
        else if (*s == 'c' || *s == 'r') {
            /* c<x>,<y> / r<x>,<y>: glide there and left / right click */
            int tx = 0, ty = 0;
            INPUT in[2];
            if (sscanf(s + 1, "%d,%d", &tx, &ty) == 2) glide(c.x, c.y, tx, ty, 80);
            ZeroMemory(in, sizeof in);
            in[0].type = in[1].type = INPUT_MOUSE;
            in[0].mi.dwFlags = *s == 'c' ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
            in[1].mi.dwFlags = *s == 'c' ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
            SendInput(2, in, sizeof in[0]);
        } else if (*s == 'F') {
            /* F<title>: bring that window to the front (Alt lets us) */
            HWND w = FindWindowA(NULL, s + 1);
            if (w) { keys(VK_MENU, 0); SetForegroundWindow(w); }
        }
    }
    InterlockedExchange(&g_done, 1);
    return 0;
}

/* the cursor's shape, looked at every ms: "#cur <ms> <handle>" on every change
   (a flicker of the cursor is faster than the frames recorded) */
static LARGE_INTEGER g_fq, g_t0;
static DWORD WINAPI cursorWatch(LPVOID p)
{
    HCURSOR last = (HCURSOR)1;
    HWND lastUnder = (HWND)1;
    (void)p;
    for (;;) {
        CURSORINFO ci = { sizeof ci };
        HCURSOR c;
        HWND under;
        LARGE_INTEGER t;
        GetCursorInfo(&ci);
        c = (ci.flags & CURSOR_SHOWING) ? ci.hCursor : NULL;
        under = GetAncestor(WindowFromPoint(ci.ptScreenPos), GA_ROOT);
        if (c != last || under != lastUnder) {
            char cls[64] = "";
            GetClassNameA(under, cls, 64);
            QueryPerformanceCounter(&t);
            EnterCriticalSection(&g_cs);
            if (g_txt) fprintf(g_txt, "#cur %.1f %p %s\n", (t.QuadPart - g_t0.QuadPart) * 1000.0 / g_fq.QuadPart,
                               (void *)c, cls);
            LeaveCriticalSection(&g_cs);
            last = c;
            lastUnder = under;
        }
        Sleep(1);
    }
    return 0;
}

/* how long Explorer (the taskbar's thread) takes to answer: "#busy <ms> <took>"
   whenever it takes more than 30 ms */
static DWORD WINAPI explorerWatch(LPVOID p)
{
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    (void)p;
    for (;;) {
        LARGE_INTEGER a, b;
        DWORD_PTR res;
        double took;
        QueryPerformanceCounter(&a);
        SendMessageTimeoutW(tb, WM_NULL, 0, 0, SMTO_NORMAL, 2000, &res);
        QueryPerformanceCounter(&b);
        took = (b.QuadPart - a.QuadPart) * 1000.0 / g_fq.QuadPart;
        if (took > 30) {
            EnterCriticalSection(&g_cs);
            if (g_txt) fprintf(g_txt, "#busy %.1f %.0f\n", (a.QuadPart - g_t0.QuadPart) * 1000.0 / g_fq.QuadPart,
                               took);
            LeaveCriticalSection(&g_cs);
        }
        Sleep(2);
    }
    return 0;
}

int main(int argc, char **argv)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    FARPROC dpi = GetProcAddress(u32, "SetProcessDPIAware");
    PFN_GLWA glwa = (PFN_GLWA)GetProcAddress(u32, "GetLayeredWindowAttributes");
    PFN_DwmFlush flush = (PFN_DwmFlush)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
    HWND tb, cov;
    RECT tr;
    HDC sdc, mdc;
    HBITMAP bmp;
    BITMAPINFO bi;
    void *bits;
    FILE *raw;
    char path[600];
    LARGE_INTEGER fq, t0, t, a;
    int W, H, x0, y0, frames = 0, tail = 0;

    if (argc < 3) { fprintf(stderr, "usage: record <outprefix> <step>...\n"); return 1; }
    if (dpi) ((BOOL (WINAPI *)(void))dpi)();
    SW = GetSystemMetrics(SM_CXSCREEN); SH = GetSystemMetrics(SM_CYSCREEN);
    tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tb || !flush) { fprintf(stderr, "no taskbar / DwmFlush\n"); return 2; }
    GetWindowRect(tb, &tr);
    g_barH = tr.bottom - tr.top;
    if (g_barH <= 0 || g_barH > 300) g_barH = 48;
    g_argc = argc; g_argv = argv;

    /* the middle of the bar (Start, Search and the first icons of a centered
       Windows 11 taskbar), the bar's height twice */
    W = 640; H = g_barH * 2;
    x0 = SW / 2 - W / 2; y0 = SH - H;
    sdc = GetDC(NULL); mdc = CreateCompatibleDC(sdc);
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(mdc, bmp);

    sprintf(path, "%s.raw", argv[1]);
    raw = fopen(path, "wb");
    sprintf(path, "%s.txt", argv[1]);
    g_txt = fopen(path, "w");
    if (!raw || !g_txt) return 3;
    fprintf(g_txt, "%d %d %d %d %d\n", W, H, x0, y0, SH - g_barH);
    InitializeCriticalSection(&g_cs);

    moveTo(SW / 3, SH / 2); Sleep(900);
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    g_fq = fq; g_t0 = t0;
    CreateThread(NULL, 0, cursorWatch, NULL, 0, NULL);
    CreateThread(NULL, 0, explorerWatch, NULL, 0, NULL);
    CreateThread(NULL, 0, script, NULL, 0, NULL);
    while (frames < 1500 && tail < 40) {
        BYTE al = 255; DWORD fl = 0; COLORREF k;
        RECT r = { 0 };
        int slide = 0, cv = 0;
        flush();
        QueryPerformanceCounter(&a);
        do QueryPerformanceCounter(&t); while ((t.QuadPart - a.QuadPart) * 1000 < fq.QuadPart * 5);
        BitBlt(mdc, 0, 0, W, H, sdc, x0, y0, SRCCOPY);
        GdiFlush();
        fwrite(bits, 4, W * H, raw);
        GetWindowRect(tb, &r);
        if (glwa && (GetWindowLongW(tb, GWL_EXSTYLE) & WS_EX_LAYERED)) glwa(tb, &k, &al, &fl);
        cov = FindWindowW(L"FastBarOverlay", NULL);
        if (cov && IsWindowVisible(cov) && GetPropW(cov, L"FastBarSeeThrough")) {
            cv = 1;
            slide = (int)(INT_PTR)GetPropW(cov, L"FastBarSlide");
        }
        {
            /* what is on top at the middle of the bar's place: the bar (1),
               another window (0) */
            POINT m = { SW / 2, SH - g_barH / 2 };
            HWND top = GetAncestor(WindowFromPoint(m), GA_ROOT);
            /* the cursor's shape (a flicker shows as it changing back and
               forth) and the tray's hidden icons (when they open) */
            CURSORINFO ci = { sizeof ci };
            HWND ov = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", NULL);
            GetCursorInfo(&ci);
            EnterCriticalSection(&g_cs);
            fprintf(g_txt, "%.1f tb=%d v%d a%d cov=%d slide=%d top=%d cur=%p ov=%d\n",
                    (t.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart, (int)r.top,
                    IsWindowVisible(tb) ? 1 : 0, al, cv, slide, top == tb,
                    (ci.flags & CURSOR_SHOWING) ? (void *)ci.hCursor : NULL,
                    ov && IsWindowVisible(ov));
        }
        LeaveCriticalSection(&g_cs);
        frames++;
        if (g_done) tail++;
    }
    fclose(raw);
    EnterCriticalSection(&g_cs);
    fclose(g_txt);
    g_txt = NULL;                      /* (cursorWatch goes on until the end) */
    LeaveCriticalSection(&g_cs);
    moveTo(SW / 2, SH / 2);
    return 0;
}
