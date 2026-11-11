/*
 * probe - drives mouse/keyboard against the running FastBar and records, once
 * per DWM frame, the state of the real taskbar and of FastBar's overlays,
 * plus a 1-pixel screen column through the bar (a "kymograph": time on x,
 * screen rows on y) to see what was really on screen.
 *
 * usage: probe.exe <scenario> <outprefix> [period_ms]
 *   app        show bar, leave it (no flyout)
 *   qs / cal   show bar, open Quick Settings (Win+A) / calendar (Win+N), leave
 *   qs-back    like qs, but come back onto the bar 150 ms after leaving
 *   qs-rapid / cal-rapid
 *              like qs / cal, then go in and out every period_ms (default 200)
 *              for 2 s, ending outside
 *   edge       from a hidden bar: rest the cursor on the bottom edge for
 *              period_ms (default 900), then leave for 900 ms; 6 times.
 *              period_ms < 0: 20 times, resting a random 50-1200 ms on the
 *              edge and a random 80-900 ms away from it.
 *   edge-jitter
 *              like edge, but the cursor slips 3 px up off the edge 60 ms
 *              after touching it and goes back down 40 ms later (a hand)
 *   edge-sweep like edge, but the mouse keeps moving left and right (1000
 *              reports a second) on the edge, then in the middle of the screen
 *   edge-wobble
 *              like edge-sweep, but moving along the edge the cursor also
 *              drifts up to 15 px up and back down, as a hand does
 *   edge-pull  a real mouse: relative moves, pulled down quickly past the edge
 *              and kept pushing down (the hook sees points below the screen)
 *   edge-pull-sweep
 *              pulled down, then moved left/right while pressing down a bit
 *              Exercises ShowDelay (Explorer's own auto-hide also reacts to a
 *              cursor resting on the edge)
 * writes <outprefix>.log (one line per frame) and <outprefix>.bmp (kymograph)
 *
 * log line: "<ms> tb=<x>,<y> v<visible> a<alpha> [ov=<x>,<y> v1 a<alpha> s]..."
 * (ov = FastBar's cover; y is where the live thumbnail in it is drawn)
 * marker lines start with "--" (cursor moves); the header gives geometry.
 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *PFN_DwmFlush)(void);
typedef BOOL (WINAPI *PFN_GLWA)(HWND, COLORREF *, BYTE *, DWORD *);
static PFN_DwmFlush pFlush;
static PFN_GLWA pGLWA;
static int SW, SH;

static void moveTo(int x, int y)       /* SendInput: goes through FastBar's mouse hook */
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

static void winInfo(FILE *f, HWND h, const char *tag)
{
    RECT r = { 0 };
    BYTE a = 255; DWORD fl = 0; COLORREF k;
    GetWindowRect(h, &r);
    /* FastBar's cover stands still: the live thumbnail in it slides, this far */
    r.top += (int)(INT_PTR)GetPropW(h, L"FastBarSlide");
    if (pGLWA && (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_LAYERED)) pGLWA(h, &k, &a, &fl);
    fprintf(f, " %s=%d,%d v%d a%d", tag, (int)r.left, (int)r.top, IsWindowVisible(h) ? 1 : 0, a);
    /* FastBar's cover is see-through: it does not hide what is under it.
       (Without this property it is up but empty, ready to show the bar.) */
    if (GetPropW(h, L"FastBarSeeThrough")) fprintf(f, " s");
}

static void saveBmp(const char *path, HDC dc, HBITMAP bmp, int w, int h)
{
    BITMAPINFO bi;
    BITMAPFILEHEADER fh;
    int stride = w * 4;
    void *px = malloc(stride * h);
    FILE *f;
    ZeroMemory(&bi, sizeof bi); ZeroMemory(&fh, sizeof fh);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    GetDIBits(dc, bmp, 0, h, px, &bi, DIB_RGB_COLORS);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader; fh.bfSize = fh.bfOffBits + stride * h;
    f = fopen(path, "wb");
    if (f) {
        fwrite(&fh, sizeof fh, 1, f); fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(px, stride * h, 1, f); fclose(f);
    }
    free(px);
}

/* sweep: a mouse moved left and right, reporting 1000 times a second, at row
   g_sweepY (-1 = stopped) */
static volatile LONG g_sweepY = -1;
static int g_sweepWobble;
/* g_sweepRel: like a real mouse, relative moves (which can push past the
   screen edge): g_sweepDx left/right, g_sweepDy down, per report */
static int g_sweepRel, g_sweepDx, g_sweepDy;
static void moveBy(int dx, int dy)
{
    INPUT in = { 0 };
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof in);
}
static DWORD WINAPI sweepThread(LPVOID p)
{
    LARGE_INTEGER fq, a, b;
    int x = SW / 3, dir = 3, n = 0, w, k = 0;
    (void)p;
    QueryPerformanceFrequency(&fq);
    for (;;) {
        LONG y = g_sweepY;
        if (y >= 0 && g_sweepRel && y == SH - 1) {
            if (++k % 200 == 0) g_sweepDx = -g_sweepDx;
            moveBy(g_sweepDx, g_sweepDy);
        } else if (y >= 0) {
            x += dir;
            if (x > SW / 3 + 300 || x < SW / 3 - 300) dir = -dir;
            /* g_sweepWobble: the hand also drifts up to that many pixels up
               and back down (triangle, 240 ms period) */
            n = (n + 1) % 240;
            w = g_sweepWobble * (n < 120 ? n : 240 - n) / 120;
            moveTo(x, y == SH - 1 ? y - w : y);
        }
        QueryPerformanceCounter(&a);
        do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) * 1000 < fq.QuadPart);
    }
    return 0;
}

/* edge scenario: no kymograph, one log line per frame, "--in"/"--out" markers
   jitter: 1 = the cursor slips off the edge and back, 2 = the mouse keeps
   moving left/right the whole time (on the edge, then in the middle) */
static int edgeScenario(const char *out, int dwell, int jitter)
{
    char path[600];
    FILE *f;
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    RECT tr;
    LARGE_INTEGER fq, t0, t;
    int x = SW / 3, cycle, inside, cycles = dwell < 0 ? 20 : 6, d, j;
    double ms, next, start;
    moveTo(x, SH / 2); Sleep(1500);            /* start hidden */
    if (jitter == 2) CreateThread(NULL, 0, sweepThread, NULL, 0, NULL);
    sprintf(path, "%s.log", out);
    f = fopen(path, "w");
    if (!f) return 3;
    GetWindowRect(tb, &tr);
    fprintf(f, "scenario edge screen %dx%d bar %d,%d,%d,%d dwell %d\n", SW, SH,
            (int)tr.left, (int)(SH - (tr.bottom - tr.top)), (int)tr.right, SH, dwell);
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    srand(12345);                              /* same "random" run every time */
    for (cycle = 0; cycle < cycles; cycle++) {
        d = dwell < 0 ? 50 + rand() % 1150 : dwell;
        for (inside = 1; inside >= 0; inside--) {
            QueryPerformanceCounter(&t);
            ms = (t.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
            InterlockedExchange(&g_sweepY, -1);
            if (inside && g_sweepRel) {
                /* a quick pull down: relative moves, the last ones far past
                   the edge (the cursor stops on it) */
                int k;
                for (k = 0; k < 12; k++) { moveBy(0, 60); Sleep(4); }
                fprintf(f, "--in %d %d\n", cycle, d);
            } else if (inside) { glide(x, SH / 2, x, SH - 1, 40); fprintf(f, "--in %d %d\n", cycle, d); }
            else {
                if (dwell < 0) d = 80 + rand() % 820;  /* random: come back early too */
                else d = 900;
                glide(x, SH - 1, x, SH / 2, 40); fprintf(f, "--out %d %d\n", cycle, d);
            }
            if (jitter == 2) InterlockedExchange(&g_sweepY, inside ? SH - 1 : SH / 2);
            next = ms + d;
            start = -1;
            j = 0;
            do {
                HWND h = NULL;
                pFlush();
                QueryPerformanceCounter(&t);
                ms = (t.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
                if (start < 0) start = ms;
                /* a hand resting on the edge: the cursor slips up a few pixels
                   and goes back down */
                if (jitter == 1 && inside && j == 0 && ms - start >= 60) { moveTo(x, SH - 4); j = 1; }
                if (jitter == 1 && inside && j == 1 && ms - start >= 100) { moveTo(x, SH - 1); j = 2; }
                fprintf(f, "%7.1f", ms);
                winInfo(f, tb, "tb");
                while ((h = FindWindowExW(NULL, h, L"FastBarOverlay", NULL)) != NULL)
                    if (IsWindowVisible(h) && GetPropW(h, L"FastBarSeeThrough")) winInfo(f, h, "ov");
                {
                    POINT c;
                    GetCursorPos(&c);
                    fprintf(f, " cur=%d,%d", (int)c.x, (int)c.y);
                }
                fprintf(f, "\n");
            } while (ms < next);
        }
    }
    InterlockedExchange(&g_sweepY, -1);
    Sleep(5);
    fclose(f);
    moveTo(SW / 2, SH / 2);
    return 0;
}

int main(int argc, char **argv)
{
    const char *sc = argc > 1 ? argv[1] : "app";
    const char *out = argc > 2 ? argv[2] : "probe";
    double period = argc > 3 ? atof(argv[3]) : 200;
    int flyout = 0, back = 0, rapid = 0;
    int barH, frames, maxF = 600, colX, regH, x, onBarY, done = 0, lastPh = 0;
    double dur;
    char path[600];
    FILE *f;
    HWND tb;
    RECT tr;
    HDC sdc, mdc;
    HBITMAP kymo;
    LARGE_INTEGER fq, t0, t;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    FARPROC dpi = GetProcAddress(u32, "SetProcessDPIAware");

    if (dpi) ((BOOL (WINAPI *)(void))dpi)();
    pGLWA = (PFN_GLWA)GetProcAddress(u32, "GetLayeredWindowAttributes");
    pFlush = (PFN_DwmFlush)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmFlush");
    SW = GetSystemMetrics(SM_CXSCREEN); SH = GetSystemMetrics(SM_CYSCREEN);
    tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tb || !pFlush) { fprintf(stderr, "no taskbar / DwmFlush\n"); return 2; }

    if (!strcmp(sc, "edge")) return edgeScenario(out, argc > 3 ? atoi(argv[3]) : 900, 0);
    if (!strcmp(sc, "edge-jitter")) return edgeScenario(out, argc > 3 ? atoi(argv[3]) : 900, 1);
    if (!strcmp(sc, "edge-sweep")) return edgeScenario(out, argc > 3 ? atoi(argv[3]) : 900, 2);
    if (!strcmp(sc, "edge-wobble")) {
        g_sweepWobble = 15;
        return edgeScenario(out, argc > 3 ? atoi(argv[3]) : 900, 2);
    }
    if (!strcmp(sc, "edge-pull")) {           /* pull down and keep pushing */
        g_sweepRel = 1; g_sweepDx = 0; g_sweepDy = 3;
        return edgeScenario(out, argc > 3 ? atoi(argv[3]) : 900, 2);
    }
    if (!strcmp(sc, "edge-pull-sweep")) {     /* pull down, then left/right, pressing down */
        g_sweepRel = 1; g_sweepDx = 3; g_sweepDy = 1;
        return edgeScenario(out, argc > 3 ? atoi(argv[3]) : 900, 2);
    }
    if (!strncmp(sc, "qs", 2)) flyout = 'A';
    else if (!strncmp(sc, "cal", 3)) flyout = 'N';
    back = strstr(sc, "-back") != NULL;
    rapid = strstr(sc, "-rapid") != NULL;
    dur = rapid ? 3000 : 1500;
    x = SW / 3;

    /* show the bar: cursor to the bottom edge (assumes a bottom taskbar) */
    moveTo(x, SH / 2); Sleep(900);
    glide(x, SH / 2, x, SH - 1, 120); Sleep(900);
    GetWindowRect(tb, &tr);
    barH = tr.bottom - tr.top;
    if (barH <= 0 || barH > 300) barH = 48;
    onBarY = SH - barH / 2;
    moveTo(x, onBarY); Sleep(300);
    if (flyout) { keys(VK_LWIN, (WORD)flyout); Sleep(1200); }

    regH = barH * 2;
    colX = x - 200;                         /* column through the bar, away from the cursor */
    if (colX < 10) colX = 10;
    sdc = GetDC(NULL); mdc = CreateCompatibleDC(sdc);
    kymo = CreateCompatibleBitmap(sdc, maxF, regH);
    SelectObject(mdc, kymo);
    PatBlt(mdc, 0, 0, maxF, regH, WHITENESS);

    sprintf(path, "%s.log", out);
    f = fopen(path, "w");
    if (!f) return 3;
    fprintf(f, "scenario %s screen %dx%d bar %d,%d,%d,%d col %d\n", sc, SW, SH,
            (int)tr.left, (int)tr.top, (int)tr.right, (int)tr.bottom, colX);
    QueryPerformanceFrequency(&fq);
    glide(x, onBarY, x, SH / 2, 40);      /* leave the bar, upwards */
    QueryPerformanceCounter(&t0);
    for (frames = 0; frames < maxF; frames++) {
        HWND h = NULL;
        double ms;
        pFlush();
        QueryPerformanceCounter(&t);
        ms = (t.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
        BitBlt(mdc, frames, 0, 1, regH, sdc, colX, SH - regH, SRCCOPY);
        fprintf(f, "%7.1f", ms);
        winInfo(f, tb, "tb");
        while ((h = FindWindowExW(NULL, h, L"FastBarOverlay", NULL)) != NULL)
            if (IsWindowVisible(h) && GetPropW(h, L"FastBarSeeThrough")) winInfo(f, h, "ov");
        fprintf(f, "\n");
        if (back && !done && ms > 150) { moveTo(x, onBarY); done = 1; fprintf(f, "--back\n"); }
        if (rapid && ms < 2000) {
            int ph = (int)(ms / period) & 1;
            if (ph != lastPh) {
                lastPh = ph;
                moveTo(x, ph ? SH - 1 : SH / 2);
                fprintf(f, ph ? "--in\n" : "--out\n");
            }
        }
        if (rapid && ms >= 2000 && lastPh) { lastPh = 0; moveTo(x, SH / 2); fprintf(f, "--out\n"); }
        if (ms > dur) { frames++; break; }
    }
    fclose(f);
    sprintf(path, "%s.bmp", out);
    saveBmp(path, mdc, kymo, frames, regH);
    if (flyout) keys(VK_ESCAPE, 0);
    moveTo(SW / 2, SH / 2);
    return 0;
}
