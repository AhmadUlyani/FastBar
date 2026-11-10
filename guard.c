/*
 * FastBarGuard - restores the taskbar if FastBar is killed.
 *
 * FastBar hides the real taskbar window. If FastBar is ended with End task
 * (or crashes in a way it cannot handle) it cannot put it back, and the
 * taskbar would stay hidden. FastBar starts this small program, which sleeps
 * on FastBar's process handle (no CPU) and, if FastBar ended without restoring
 * the taskbar ("Running=1" still in FastBar.ini), restores it.
 *
 *   FastBarGuard.exe --spawn <pid>   start the guard detached, then exit
 *   FastBarGuard.exe --watch <pid>   the guard itself
 *
 * "--spawn" makes the guard a process without a living parent, so that
 * "End process tree" on FastBar does not end the guard too.
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

static WCHAR g_exe[MAX_PATH], g_ini[MAX_PATH];

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

/* FastBarTap.dll (see-through taskbar background) in Explorer: original
   background back. Posted, so it does not matter that FastBar is gone. */
static void tapOff(void)
{
    HWND w;
    for (w = FindWindowExW(HWND_MESSAGE, NULL, L"FastBarTapWnd", NULL); w;
         w = FindWindowExW(HWND_MESSAGE, w, L"FastBarTapWnd", NULL))
    {
        PostMessageW(w, WM_APP + 1, 0, 0);     /* original background */
        PostMessageW(w, WM_APP + 2, 0, 0);     /* the bar is not pinned anymore */
    }
}

static void restoreTaskbar(void)
{
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);
    APPBARDATA ab = { sizeof ab };
    if (tb) {
        LONG ex = GetWindowLongW(tb, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) {              /* FastBar was killed in the middle of a slide */
            SetLayeredWindowAttributes(tb, 0, 255, LWA_ALPHA);
            SetWindowLongW(tb, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
        }
        ShowWindow(tb, SW_SHOWNOACTIVATE);
        ab.hWnd = tb;
        ab.lParam = GetPrivateProfileIntW(L"FastBar", L"OriginalAutoHide", 0, g_ini) ? ABS_AUTOHIDE
                                                                                  : ABS_ALWAYSONTOP;
        SHAppBarMessage(ABM_SETSTATE, &ab);
    }
    WritePrivateProfileStringW(L"FastBar", L"Running", L"0", g_ini);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show)
{
    const WCHAR *arg;
    WCHAR *slash, *p;
    (void)hi; (void)prev; (void)cmd; (void)show;

    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    lstrcpyW(g_ini, g_exe);
    slash = g_ini;
    for (p = g_ini; *p; p++) if (*p == L'\\') slash = p + 1;
    lstrcpyW(slash, L"FastBar.ini");               /* FastBar.ini sits next to both exes */

    if ((arg = argAfter(GetCommandLineW(), L" --spawn ")) != NULL) {
        WCHAR line[MAX_PATH + 32];
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi;
        wsprintfW(line, L"\"%s\" --watch %lu", g_exe, parseNum(arg));
        if (CreateProcessW(g_exe, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return 0;
    }
    if ((arg = argAfter(GetCommandLineW(), L" --watch ")) != NULL) {
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, parseNum(arg));
        if (!proc) return 1;
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   /* tiny while waiting */
        WaitForSingleObject(proc, INFINITE);
        CloseHandle(proc);
        tapOff();
        if (GetPrivateProfileIntW(L"FastBar", L"Running", 0, g_ini))   /* FastBar did not clean up */
            restoreTaskbar();
        return 0;
    }
    return 0;
}
