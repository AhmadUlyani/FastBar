/*
 * FastBarTap.dll - see-through taskbar background (64-bit, loaded into Explorer)
 *
 * Windows 11 draws the taskbar's background in its XAML content: a Rectangle
 * named "BackgroundFill" (and a 1 px "BackgroundStroke" line on top) inside
 * Taskbar.TaskbarFrame. It can only be changed from inside Explorer, the way
 * TranslucentTB does it: this DLL is loaded into Explorer through Windows'
 * XAML diagnostics interface (InitializeXamlDiagnosticsEx), watches the
 * taskbar's visual tree, and sets the Fill of those rectangles to a
 * transparent (or tinted) brush. Clearing the property gives the original
 * look back. Nothing else in Explorer is touched.
 *
 * FastBar talks to it through message-only windows of class FastBarTapWnd
 * (one per XAML thread in Explorer):
 *   TAP_MSG_SET, wParam 1, lParam = ARGB color  -> background in that color
 *   TAP_MSG_SET, wParam 0                       -> original background
 *   TAP_MSG_PIN, wParam 1 / 0                   -> keep the bar in place, or not
 *
 * Pin: when Start or a flyout closes, Explorer hides an auto-hide taskbar by
 * itself even with the cursor on it (Windows does this without FastBar too),
 * and no message from outside stops it. While pinned (FastBar: the cursor is
 * on the shown bar), the move and the hiding of its content are refused where
 * Explorer asks for them, so the real bar simply stays.
 *
 * rundll32 entry point (FastBar starts 64-bit rundll32 for this):
 *   rundll32.exe FastBarTap.dll,Inject <ARGB hex | off>
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <ocidl.h>
#include <xamlom.h>

#define TAP_CLASS   L"FastBarTapWnd"
#define TAP_MSG_SET (WM_APP + 1)
#define TAP_MSG_PIN (WM_APP + 2)

/* {6E3A1F42-8C2B-4D5E-9A71-2F0B6C4D8E13} */
DEFINE_GUID(CLSID_FastBarTap, 0x6e3a1f42, 0x8c2b, 0x4d5e, 0x9a, 0x71, 0x2f, 0x0b, 0x6c, 0x4d, 0x8e, 0x13);

static HINSTANCE g_inst;

#ifdef TAP_DEBUG
static void dbg(const char *fmt, ...)
{
    char b[512];
    DWORD n;
    va_list ap;
    HANDLE f;
    va_start(ap, fmt);
    n = wvsprintfA(b, fmt, ap);
    va_end(ap);
    f = CreateFileA(TAP_DEBUG, FILE_APPEND_DATA, 7, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, b, n, &n, NULL); CloseHandle(f); }
}
#define DBG(x) dbg x
#else
#define DBG(x)
#endif

/* ===================================================================== */
/* state (all on Explorer's XAML thread(s), see OnVisualTreeChange)       */
/* ===================================================================== */
static IVisualTreeService *g_vts;
static IXamlDiagnostics *g_diag;
static volatile LONG g_on = 0;          /* FastBar switches it on, TAP_MSG_SET */
static volatile LONG g_color = 0;       /* ARGB */

/* elements inside a Taskbar.TaskbarFrame (to tell the taskbar's rectangles
   from same-named ones elsewhere in Explorer) */
static InstanceHandle *g_frameSet;
static int g_frameN, g_frameCap;

#define MAX_TARGETS 32
typedef struct {
    InstanceHandle h;
    DWORD thread;
    unsigned int fillIdx;   /* index of the Fill property, ~0 = not known yet */
    IInspectable *orig;     /* Explorer's own brush (a local value), NULL = none */
    int set;                /* our brush is set */
} Target;
static Target g_t[MAX_TARGETS];
static int g_tn;

/* our brush per XAML thread. The object is held: a handle alone goes stale
   once nothing in the tree uses the brush (after "off") */
typedef struct { DWORD thread; LONG color; IInspectable *obj; } Brush;
static Brush g_brushes[8];
static int g_brushN;

static int inFrame(InstanceHandle h)
{
    int i;
    for (i = 0; i < g_frameN; i++) if (g_frameSet[i] == h) return 1;
    return 0;
}
static void frameAdd(InstanceHandle h)
{
    if (inFrame(h)) return;
    if (g_frameN == g_frameCap) {
        int cap = g_frameCap ? g_frameCap * 2 : 1024;
        InstanceHandle *n = g_frameSet
            ? (InstanceHandle *)HeapReAlloc(GetProcessHeap(), 0, g_frameSet, cap * sizeof *n)
            : (InstanceHandle *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof *n);
        if (!n) return;
        g_frameSet = n;
        g_frameCap = cap;
    }
    g_frameSet[g_frameN++] = h;
}
static void frameRemove(InstanceHandle h)
{
    int i;
    for (i = 0; i < g_frameN; i++)
        if (g_frameSet[i] == h) { g_frameSet[i] = g_frameSet[--g_frameN]; return; }
}

static int bstrIs(BSTR b, const WCHAR *s)
{
    return b && lstrcmpW(b, s) == 0;
}

/* the transparent / tinted brush for the calling thread (XAML objects belong to their thread) */
static IUnknown *identity(IInspectable *o)
{
    IUnknown *u = NULL;
    if (o && SUCCEEDED(IInspectable_QueryInterface(o, &IID_IUnknown, (void **)&u))) IUnknown_Release(u);
    return u;                               /* only compared, the object is held elsewhere */
}

/* handle of the transparent / tinted brush for the calling thread (XAML
   objects belong to their thread) */
static InstanceHandle brushFor(void)
{
    DWORD th = GetCurrentThreadId();
    int i;
    WCHAR v[16];
    BSTR type, val;
    InstanceHandle b = 0;
    Brush *e = NULL;
    for (i = 0; i < g_brushN; i++) if (g_brushes[i].thread == th) e = &g_brushes[i];
    if (!e && g_brushN < 8) {
        e = &g_brushes[g_brushN++];
        e->thread = th;
        e->obj = NULL;
    }
    if (!e || !g_diag) return 0;
    if (e->obj && e->color == g_color &&
        SUCCEEDED(IXamlDiagnostics_RegisterInstance(g_diag, e->obj, &b)) && b)
        return b;
    wsprintfW(v, L"#%08X", (unsigned)g_color);
    type = SysAllocString(L"Windows.UI.Xaml.Media.SolidColorBrush");
    val = SysAllocString(v);
    if (FAILED(IVisualTreeService_CreateInstance(g_vts, type, val, &b))) b = 0;
    SysFreeString(type);
    SysFreeString(val);
    DBG(("brush %ls -> %I64x\r\n", v, (unsigned long long)b));
    if (b) {
        IInspectable *o = NULL;
        if (SUCCEEDED(IXamlDiagnostics_GetIInspectableFromHandle(g_diag, b, &o)) && o) {
            if (e->obj) IInspectable_Release(e->obj);
            e->obj = o;
            e->color = g_color;
        }
    }
    return b;
}

static void freeChain(unsigned int ns, PropertyChainSource *src, unsigned int np, PropertyChainValue *pv)
{
    unsigned int i;
    for (i = 0; i < ns; i++) {
        SysFreeString(src[i].TargetType);
        SysFreeString(src[i].Name);
        SysFreeString(src[i].SrcInfo.FileName);
        SysFreeString(src[i].SrcInfo.Hash);
    }
    for (i = 0; i < np; i++) {
        SysFreeString(pv[i].Type);
        SysFreeString(pv[i].DeclaringType);
        SysFreeString(pv[i].ValueType);
        SysFreeString(pv[i].ItemType);
        SysFreeString(pv[i].Value);
        SysFreeString(pv[i].PropertyName);
    }
    CoTaskMemFree(src);
    CoTaskMemFree(pv);
}

static int isOurBrush(IInspectable *o)
{
    int i;
    IUnknown *u = identity(o);
    for (i = 0; i < g_brushN; i++)
        if (g_brushes[i].obj && identity(g_brushes[i].obj) == u) return 1;
    return 0;
}

/* index of the Fill property, and the brush set on it as a local value
   (Explorer sets the taskbar's background brush that way, so clearing our
   value would leave no background at all: the original is put back instead) */
static unsigned int fillInfo(InstanceHandle h, InstanceHandle *local)
{
    unsigned int ns = 0, np = 0, i, idx = ~0u;
    PropertyChainSource *src = NULL;
    PropertyChainValue *pv = NULL;
    *local = 0;
    if (FAILED(IVisualTreeService_GetPropertyValuesChain(g_vts, h, &ns, &src, &np, &pv))) return ~0u;
    for (i = 0; i < np; i++) {
        if (!bstrIs(pv[i].PropertyName, L"Fill")) continue;
        DBG(("fill idx %u val '%ls' vtype '%ls' bits %I64x src %d\r\n", pv[i].Index,
             pv[i].Value ? pv[i].Value : L"", pv[i].ValueType ? pv[i].ValueType : L"",
             (unsigned long long)pv[i].MetadataBits,
             pv[i].PropertyChainIndex < ns ? (int)src[pv[i].PropertyChainIndex].Source : -1));
        if (idx == ~0u) idx = pv[i].Index;
        if (!*local && pv[i].PropertyChainIndex < ns &&
            src[pv[i].PropertyChainIndex].Source == BaseValueSourceLocal &&
            (pv[i].MetadataBits & IsValueHandle) && pv[i].Value) {
            const WCHAR *c = pv[i].Value;
            InstanceHandle v = 0;
            for (; *c >= L'0' && *c <= L'9'; c++) v = v * 10 + (*c - L'0');
            *local = v;
        }
    }
    freeChain(ns, src, np, pv);
    return idx;
}

static void applyTarget(Target *t)
{
    HRESULT hr;
    InstanceHandle cur;
    if (t->thread != GetCurrentThreadId()) return;
    t->fillIdx = fillInfo(t->h, &cur);
    if (t->fillIdx == ~0u) return;
    /* keep Explorer's own brush (first time, or a new one it put in since,
       e.g. after a theme change); holding it keeps it alive */
    if (cur && g_diag) {
        IInspectable *o = NULL;
        if (SUCCEEDED(IXamlDiagnostics_GetIInspectableFromHandle(g_diag, cur, &o)) && o) {
            if (isOurBrush(o)) {
                IInspectable_Release(o);
            } else {
                if (t->orig) IInspectable_Release(t->orig);
                t->orig = o;
            }
        }
    }
    if (g_on) {
        InstanceHandle b = brushFor();
        if (!b) return;
        hr = IVisualTreeService_SetProperty(g_vts, t->h, b, t->fillIdx);
        t->set = SUCCEEDED(hr);
        DBG(("set %I64x idx %u hr %lx\r\n", (unsigned long long)t->h, t->fillIdx, hr));
    } else if (t->set) {
        InstanceHandle o = 0;
        if (t->orig && g_diag && SUCCEEDED(IXamlDiagnostics_RegisterInstance(g_diag, t->orig, &o)) && o)
            hr = IVisualTreeService_SetProperty(g_vts, t->h, o, t->fillIdx);
        else
            hr = IVisualTreeService_ClearProperty(g_vts, t->h, t->fillIdx);
        t->set = 0;
        DBG(("restore %I64x orig %I64x hr %lx\r\n", (unsigned long long)t->h,
             (unsigned long long)o, hr));
    }
}

/* ---------- pin: the bar stays where it is ---------- */
static volatile LONG g_pin;
static WNDPROC g_trayOrig, g_bridgeOrig;
static HWND g_tray, g_bridge;

static void pinPos(HWND h, WINDOWPOS *wp, int moves)
{
    wp->flags &= ~SWP_HIDEWINDOW;
    if (moves && !(wp->flags & SWP_NOMOVE)) {
        RECT r;
        /* (a real change of size, e.g. of the display, goes through) */
        if ((wp->flags & SWP_NOSIZE) ||
            (GetWindowRect(h, &r) && wp->cx == r.right - r.left && wp->cy == r.bottom - r.top)) {
            wp->flags |= SWP_NOMOVE;
            DBG(("pin: move refused\r\n"));
        }
    }
}
static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_WINDOWPOSCHANGING && g_pin) pinPos(h, (WINDOWPOS *)l, 1);
    return CallWindowProcW(g_trayOrig, h, m, w, l);
}
static LRESULT CALLBACK BridgeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_WINDOWPOSCHANGING && g_pin) pinPos(h, (WINDOWPOS *)l, 0);
    return CallWindowProcW(g_bridgeOrig, h, m, w, l);
}
/* stays installed: the DLL is never unloaded, and unpinned it only passes on */
static void pinInstall(void)
{
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL), br;
    if (!tray) return;
    if (!g_trayOrig || g_tray != tray) {
        g_tray = tray;
        g_trayOrig = (WNDPROC)SetWindowLongPtrW(tray, GWLP_WNDPROC, (LONG_PTR)TrayProc);
    }
    br = FindWindowExW(tray, NULL, L"Windows.UI.Composition.DesktopWindowContentBridge", NULL);
    if (br && (!g_bridgeOrig || g_bridge != br)) {
        g_bridge = br;
        g_bridgeOrig = (WNDPROC)SetWindowLongPtrW(br, GWLP_WNDPROC, (LONG_PTR)BridgeProc);
    }
}

/* ---------- message window: FastBar switches the look ---------- */
static LRESULT CALLBACK TapWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == TAP_MSG_SET) {
        int i;
        InterlockedExchange(&g_on, w ? 1 : 0);
        if (w) InterlockedExchange(&g_color, (LONG)l);
        for (i = 0; i < g_tn; i++) applyTarget(&g_t[i]);
        return 1;
    }
    if (m == TAP_MSG_PIN) {
        static volatile LONG busy;
        if (w && !InterlockedExchange(&busy, 1)) {
            pinInstall();
            InterlockedExchange(&busy, 0);
        }
        InterlockedExchange(&g_pin, w ? 1 : 0);
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD g_wndThreads[8];
static int g_wndThreadN;
static void ensureWindow(void)
{
    static int registered;
    DWORD th = GetCurrentThreadId();
    int i;
    for (i = 0; i < g_wndThreadN; i++) if (g_wndThreads[i] == th) return;
    if (g_wndThreadN == 8) return;
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = TapWndProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = TAP_CLASS;
        RegisterClassW(&wc);
        registered = 1;
    }
    if (CreateWindowExW(0, TAP_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL))
        g_wndThreads[g_wndThreadN++] = th;
}

/* ===================================================================== */
/* IVisualTreeServiceCallback2                                            */
/* ===================================================================== */
typedef struct {
    IObjectWithSite ows;
    IVisualTreeServiceCallback2 cb;
    IUnknown *site;
} Tap;
static Tap g_tap;

static HRESULT STDMETHODCALLTYPE Cb_QI(IVisualTreeServiceCallback2 *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IVisualTreeServiceCallback) ||
        IsEqualIID(riid, &IID_IVisualTreeServiceCallback2)) {
        *ppv = This;
        return S_OK;
    }
    if (IsEqualIID(riid, &IID_IObjectWithSite)) { *ppv = &g_tap.ows; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Cb_AddRef(IVisualTreeServiceCallback2 *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE Cb_Release(IVisualTreeServiceCallback2 *This) { (void)This; return 1; }

static HRESULT STDMETHODCALLTYPE Cb_OnVisualTreeChange(IVisualTreeServiceCallback2 *This,
    ParentChildRelation rel, VisualElement el, VisualMutationType mt)
{
    int i;
    (void)This;
    ensureWindow();
    if (mt == Add) {
        if (bstrIs(el.Type, L"Taskbar.TaskbarFrame") || (rel.Parent && inFrame(rel.Parent))) {
            frameAdd(el.Handle);
            DBG(("add %ls '%ls' %I64x\r\n", el.Type ? el.Type : L"", el.Name ? el.Name : L"",
                 (unsigned long long)el.Handle));
            if ((bstrIs(el.Name, L"BackgroundFill") || bstrIs(el.Name, L"BackgroundStroke")) &&
                bstrIs(el.Type, L"Windows.UI.Xaml.Shapes.Rectangle") && g_tn < MAX_TARGETS) {
                for (i = 0; i < g_tn && g_t[i].h != el.Handle; i++) ;
                if (i == g_tn) {
                    g_t[i].h = el.Handle;
                    g_t[i].thread = GetCurrentThreadId();
                    g_t[i].fillIdx = ~0u;
                    g_t[i].orig = NULL;
                    g_t[i].set = 0;
                    g_tn++;
                }
                if (g_on) applyTarget(&g_t[i]);
            }
        }
    } else {
        frameRemove(el.Handle);
        for (i = 0; i < g_tn; i++)
            if (g_t[i].h == el.Handle) {
                if (g_t[i].orig) IInspectable_Release(g_t[i].orig);
                g_t[i] = g_t[--g_tn];
                break;
            }
    }
    SysFreeString(el.Type);
    SysFreeString(el.Name);
    SysFreeString(el.SrcInfo.FileName);
    SysFreeString(el.SrcInfo.Hash);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Cb_OnElementStateChanged(IVisualTreeServiceCallback2 *This,
    InstanceHandle el, VisualElementState st, LPCWSTR ctx)
{
    (void)This; (void)el; (void)st; (void)ctx;
    return S_OK;
}
static IVisualTreeServiceCallback2Vtbl g_cbVtbl = {
    Cb_QI, Cb_AddRef, Cb_Release, Cb_OnVisualTreeChange, Cb_OnElementStateChanged
};

/* ===================================================================== */
/* IObjectWithSite: XAML diagnostics hands us its site after loading us   */
/* ===================================================================== */
static HRESULT STDMETHODCALLTYPE Ows_QI(IObjectWithSite *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IObjectWithSite)) {
        *ppv = This;
        return S_OK;
    }
    return Cb_QI(&g_tap.cb, riid, ppv);
}
static ULONG STDMETHODCALLTYPE Ows_AddRef(IObjectWithSite *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE Ows_Release(IObjectWithSite *This) { (void)This; return 1; }

/* Advising from the thread that calls SetSite hangs: do it from a new one */
static DWORD WINAPI AdviseThread(LPVOID p)
{
    HRESULT hr;
    (void)p;
    hr = IVisualTreeService_AdviseVisualTreeChange(g_vts, (IVisualTreeServiceCallback *)&g_tap.cb);
    DBG(("advise hr %lx\r\n", hr));
    (void)hr;
    return 0;
}

static HRESULT STDMETHODCALLTYPE Ows_SetSite(IObjectWithSite *This, IUnknown *site)
{
    (void)This;
    DBG(("SetSite %p thread %lu\r\n", site, GetCurrentThreadId()));
    if (!site) return S_OK;
    if (g_vts) return S_OK;                 /* already watching */
    IUnknown_AddRef(site);
    g_tap.site = site;
    if (FAILED(IUnknown_QueryInterface(site, &IID_IVisualTreeService, (void **)&g_vts))) {
        g_vts = NULL;
        return E_FAIL;
    }
    if (FAILED(IUnknown_QueryInterface(site, &IID_IXamlDiagnostics, (void **)&g_diag))) g_diag = NULL;
    {
        HANDLE t = CreateThread(NULL, 0, AdviseThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Ows_GetSite(IObjectWithSite *This, REFIID riid, void **ppv)
{
    (void)This;
    if (!g_tap.site) { *ppv = NULL; return E_FAIL; }
    return IUnknown_QueryInterface(g_tap.site, riid, ppv);
}
static IObjectWithSiteVtbl g_owsVtbl = { Ows_QI, Ows_AddRef, Ows_Release, Ows_SetSite, Ows_GetSite };

/* ---------- class factory ---------- */
static HRESULT STDMETHODCALLTYPE Cf_QI(IClassFactory *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = This;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Cf_AddRef(IClassFactory *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE Cf_Release(IClassFactory *This) { (void)This; return 1; }
static HRESULT STDMETHODCALLTYPE Cf_CreateInstance(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)This;
    if (outer) return CLASS_E_NOAGGREGATION;
    return Ows_QI(&g_tap.ows, riid, ppv);
}
static HRESULT STDMETHODCALLTYPE Cf_LockServer(IClassFactory *This, BOOL lock) { (void)This; (void)lock; return S_OK; }
static IClassFactoryVtbl g_cfVtbl = { Cf_QI, Cf_AddRef, Cf_Release, Cf_CreateInstance, Cf_LockServer };
static IClassFactory g_cf = { &g_cfVtbl };

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!IsEqualCLSID(clsid, &CLSID_FastBarTap)) return CLASS_E_CLASSNOTAVAILABLE;
    return Cf_QI(&g_cf, riid, ppv);
}
/* once in Explorer it stays (its XAML keeps pointers to our callback) */
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

/* ===================================================================== */
/* rundll32 FastBarTap.dll,Inject <ARGB hex>                              */
/* ===================================================================== */
typedef HRESULT (WINAPI *PFN_IXDE)(PCWSTR, DWORD, PCWSTR, PCWSTR, CLSID, PCWSTR);

static LONG parseHex(const WCHAR *s)
{
    unsigned long v = 0;
    while (*s == L' ' || *s == L'#') s++;
    for (; *s; s++) {
        int d;
        if (*s >= L'0' && *s <= L'9') d = *s - L'0';
        else if (*s >= L'a' && *s <= L'f') d = *s - L'a' + 10;
        else if (*s >= L'A' && *s <= L'F') d = *s - L'A' + 10;
        else break;
        v = v * 16 + d;
    }
    return (LONG)v;
}

__declspec(dllexport) void CALLBACK InjectW(HWND hw, HINSTANCE hi, LPWSTR cmd, int show)
{
    WCHAR path[MAX_PATH], conn[64];
    DWORD pid = 0;
    HWND tb, w;
    HMODULE x;
    PFN_IXDE ixde;
    LONG color = parseHex(cmd ? cmd : L"0");
    /* "off": only loaded, for the pin: the taskbar keeps its own background */
    int on = !(cmd && cmd[0] == L'o'), i;
    (void)hw; (void)hi; (void)show;
    tb = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!tb) return;
    /* already in this Explorer: just switch the look */
    w = FindWindowExW(HWND_MESSAGE, NULL, TAP_CLASS, NULL);
    if (w) {
        for (; w; w = FindWindowExW(HWND_MESSAGE, w, TAP_CLASS, NULL))
            SendMessageTimeoutW(w, TAP_MSG_SET, on, color, SMTO_ABORTIFHUNG, 2000, NULL);
        return;
    }
    GetWindowThreadProcessId(tb, &pid);
    GetModuleFileNameW(g_inst, path, MAX_PATH);
    x = LoadLibraryExW(L"Windows.UI.Xaml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ixde = x ? (PFN_IXDE)GetProcAddress(x, "InitializeXamlDiagnosticsEx") : NULL;
    if (!ixde) return;
    /* the initial color goes in through the environment-free way: once the
       DLL's window exists in Explorer, send it */
    for (i = 0; i < 5; i++) {
        HRESULT hr;
        wsprintfW(conn, L"VisualDiagConnection%d", i + 1);
        hr = ixde(conn, pid, NULL, path, CLSID_FastBarTap, NULL);
        DBG(("ixde %ls hr %lx\r\n", conn, hr));
        if (SUCCEEDED(hr)) break;
        Sleep(300);
    }
    for (i = 0; i < 50; i++) {             /* up to 5 s for the tree walk */
        w = FindWindowExW(HWND_MESSAGE, NULL, TAP_CLASS, NULL);
        if (w) break;
        Sleep(100);
    }
    for (; w; w = FindWindowExW(HWND_MESSAGE, w, TAP_CLASS, NULL))
        SendMessageTimeoutW(w, TAP_MSG_SET, on, color, SMTO_ABORTIFHUNG, 2000, NULL);
    /* XAML threads that came up meanwhile have their window by now */
    Sleep(800);
    for (w = FindWindowExW(HWND_MESSAGE, NULL, TAP_CLASS, NULL); w;
         w = FindWindowExW(HWND_MESSAGE, w, TAP_CLASS, NULL))
        SendMessageTimeoutW(w, TAP_MSG_SET, on, color, SMTO_ABORTIFHUNG, 2000, NULL);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = h;
        g_tap.ows.lpVtbl = &g_owsVtbl;
        g_tap.cb.lpVtbl = &g_cbVtbl;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
