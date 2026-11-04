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
 * writes <outprefix>.log (one line per frame) and <outprefix>.bmp (kymograph)
 *
 * log line: "<ms> tb=<x>,<y> v<visible> a<alpha> [ov=<x>,<y> v1 a<alpha>]..."
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
    if (pGLWA && (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_LAYERED)) pGLWA(h, &k, &a, &fl);
    fprintf(f, " %s=%d,%d v%d a%d", tag, (int)r.left, (int)r.top, IsWindowVisible(h) ? 1 : 0, a);
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
        while ((h = FindWindowExW(NULL, h, L"FastBarSnapshot", NULL)) != NULL)
            if (IsWindowVisible(h)) winInfo(f, h, "ov");
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
