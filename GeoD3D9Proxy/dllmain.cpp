// ============================================================================
//  geod3d9.dll  —  D3D9 -> D3D9Ex upgrading proxy for GeoVrExport
// ============================================================================
//
//  WHY THIS EXISTS
//  ---------------
//  Legacy D3D9 games (e.g. Dragon Age: Origins) create a *plain* IDirect3DDevice9
//  via Direct3DCreate9 + CreateDevice. A plain (non-Ex) device cannot create or open
//  shared textures, so GeoVrExport is forced onto a slow CPU readback path. Only an
//  IDirect3DDevice9Ex (created via Direct3DCreate9Ex + CreateDeviceEx) supports the
//  GPU-side shared render-target that GeoVrExport's fast path needs.
//
//  This proxy is installed AS THE GAME'S d3d9.dll. It transparently upgrades the
//  game's Direct3DCreate9 -> Direct3DCreate9Ex and CreateDevice -> CreateDeviceEx,
//  then chainloads ReShade (kept named d3d9.dll inside a ReShade\ subfolder). Because
//  the upgrade happens ABOVE ReShade, ReShade itself ends up calling CreateDeviceEx,
//  so ReShade's device wrapper is marked "extended" and GeoVrExport's
//  QueryInterface(IDirect3DDevice9Ex) succeeds -> GPU shared path, zero readback.
//
//  LOAD ORDER (the whole point):
//      game -> d3d9.dll (THIS proxy) -> ReShade\d3d9.dll (chainloaded) -> system d3d9
//
//  The proxy chainloads ReShade first so ReShade's inline hooks on the *system* d3d9
//  exports are in place before we resolve and call them. Our Direct3DCreate9 then calls
//  the (hooked) system Direct3DCreate9Ex, so ReShade wraps the extended factory; the
//  game's CreateDevice flows through our wrapper -> ReShade's CreateDeviceEx -> an
//  extended device wrapper that exposes IDirect3DDevice9Ex.
//
//  This component does NOT touch the GeoVrExport / SuperVrExport addons in any way.
// ============================================================================

#include <windows.h>
#include <d3d9.h>
#include <string>

// ── Resolved system d3d9 entry points (ReShade-hooked once ReShade is loaded) ──
static HMODULE g_sys_d3d9 = nullptr;   // C:\Windows\System32\d3d9.dll
static HMODULE g_chain    = nullptr;   // chainloaded ReShade (named d3d9.dll, in a subfolder)
static bool    g_inited   = false;

typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT);
typedef HRESULT     (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
typedef int         (WINAPI *PFN_BeginEvent)(D3DCOLOR, LPCWSTR);
typedef int         (WINAPI *PFN_EndEvent)(void);
typedef void        (WINAPI *PFN_SetMarker)(D3DCOLOR, LPCWSTR);
typedef void        (WINAPI *PFN_SetRegion)(D3DCOLOR, LPCWSTR);
typedef BOOL        (WINAPI *PFN_QueryRepeatFrame)(void);
typedef void        (WINAPI *PFN_SetOptions)(DWORD);
typedef DWORD       (WINAPI *PFN_GetStatus)(void);

static PFN_Direct3DCreate9   p_Create9   = nullptr;
static PFN_Direct3DCreate9Ex p_Create9Ex = nullptr;

// ── Resolve our own folder so we can locate ReShade and geod3d9.ini ─────────────
static std::string self_folder()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&self_folder), &self);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : s.substr(0, slash + 1);
}

// ── One-time init: chainload ReShade, then resolve the system d3d9 exports ──────
static bool ensure_init()
{
    if (g_inited) return g_sys_d3d9 != nullptr;
    g_inited = true;

    const std::string folder = self_folder();

    // ReShade target is configurable via geod3d9.ini ([geod3d9] ReShade=...).
    // Default: ReShade\d3d9.dll  — ReShade MUST keep the d3d9.dll name to detect the
    // API, but a subfolder avoids any clash with this proxy in the game folder.
    char rel[MAX_PATH] = {};
    GetPrivateProfileStringA("geod3d9", "ReShade", "ReShade\\d3d9.dll",
                             rel, MAX_PATH, (folder + "geod3d9.ini").c_str());

    // Chainload ReShade FIRST so its system-d3d9 export hooks are installed before we
    // resolve/call them. (Absolute path keeps the loader from finding us again.)
    g_chain = LoadLibraryA((folder + rel).c_str());

    char sysdir[MAX_PATH] = {};
    GetSystemDirectoryA(sysdir, MAX_PATH);
    g_sys_d3d9 = LoadLibraryA((std::string(sysdir) + "\\d3d9.dll").c_str());
    if (!g_sys_d3d9) return false;

    p_Create9   = reinterpret_cast<PFN_Direct3DCreate9>  (GetProcAddress(g_sys_d3d9, "Direct3DCreate9"));
    p_Create9Ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(GetProcAddress(g_sys_d3d9, "Direct3DCreate9Ex"));
    return true;
}

// ============================================================================
//  Factory wrapper: forwards everything to the real IDirect3D9Ex, but rewrites
//  CreateDevice -> CreateDeviceEx so the produced device is extended.
// ============================================================================
class ProxyD3D9Ex final : public IDirect3D9Ex
{
    IDirect3D9Ex* m_real;
    LONG          m_ref;
public:
    explicit ProxyD3D9Ex(IDirect3D9Ex* real) : m_real(real), m_ref(1) {}

    // ── IUnknown ──
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9) || riid == __uuidof(IDirect3D9Ex)) {
            *ppv = static_cast<IDirect3D9Ex*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG WINAPI Release() override
    {
        const ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { m_real->Release(); delete this; }
        return r;
    }

    // ── IDirect3D9 (straight passthrough) ──
    HRESULT WINAPI RegisterSoftwareDevice(void* p) override { return m_real->RegisterSoftwareDevice(p); }
    UINT    WINAPI GetAdapterCount() override { return m_real->GetAdapterCount(); }
    HRESULT WINAPI GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* id) override { return m_real->GetAdapterIdentifier(a, f, id); }
    UINT    WINAPI GetAdapterModeCount(UINT a, D3DFORMAT f) override { return m_real->GetAdapterModeCount(a, f); }
    HRESULT WINAPI EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* pm) override { return m_real->EnumAdapterModes(a, f, m, pm); }
    HRESULT WINAPI GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* pm) override { return m_real->GetAdapterDisplayMode(a, pm); }
    HRESULT WINAPI CheckDeviceType(UINT a, D3DDEVTYPE dt, D3DFORMAT af, D3DFORMAT bf, BOOL w) override { return m_real->CheckDeviceType(a, dt, af, bf, w); }
    HRESULT WINAPI CheckDeviceFormat(UINT a, D3DDEVTYPE dt, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT cf) override { return m_real->CheckDeviceFormat(a, dt, af, u, rt, cf); }
    HRESULT WINAPI CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE dt, D3DFORMAT sf, BOOL w, D3DMULTISAMPLE_TYPE mt, DWORD* q) override { return m_real->CheckDeviceMultiSampleType(a, dt, sf, w, mt, q); }
    HRESULT WINAPI CheckDepthStencilMatch(UINT a, D3DDEVTYPE dt, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) override { return m_real->CheckDepthStencilMatch(a, dt, af, rf, df); }
    HRESULT WINAPI CheckDeviceFormatConversion(UINT a, D3DDEVTYPE dt, D3DFORMAT sf, D3DFORMAT tf) override { return m_real->CheckDeviceFormatConversion(a, dt, sf, tf); }
    HRESULT WINAPI GetDeviceCaps(UINT a, D3DDEVTYPE dt, D3DCAPS9* c) override { return m_real->GetDeviceCaps(a, dt, c); }
    HMONITOR WINAPI GetAdapterMonitor(UINT a) override { return m_real->GetAdapterMonitor(a); }

    // ── THE UPGRADE: CreateDevice -> CreateDeviceEx ──
    HRESULT WINAPI CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pp,
                                IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        D3DDISPLAYMODEEX  fsmode = {};
        D3DDISPLAYMODEEX* pfs    = nullptr;
        if (pp != nullptr && !pp->Windowed) {
            fsmode.Size             = sizeof(D3DDISPLAYMODEEX);
            fsmode.Width            = pp->BackBufferWidth;
            fsmode.Height           = pp->BackBufferHeight;
            fsmode.RefreshRate      = pp->FullScreen_RefreshRateInHz;
            fsmode.Format           = pp->BackBufferFormat;
            fsmode.ScanLineOrdering  = D3DSCANLINEORDERING_PROGRESSIVE;
            pfs = &fsmode;
        }

        IDirect3DDevice9Ex* exDev = nullptr;
        HRESULT hr = m_real->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp, pfs, &exDev);
        if (SUCCEEDED(hr) && exDev != nullptr) {
            *ppReturnedDeviceInterface = exDev; // IDirect3DDevice9Ex* is-a IDirect3DDevice9*
            return hr;
        }
        // Never make the game worse: if the Ex upgrade fails for any reason, fall back
        // to a normal device exactly as the game asked.
        return m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);
    }

    // ── IDirect3D9Ex (passthrough) ──
    UINT    WINAPI GetAdapterModeCountEx(UINT a, const D3DDISPLAYMODEFILTER* f) override { return m_real->GetAdapterModeCountEx(a, f); }
    HRESULT WINAPI EnumAdapterModesEx(UINT a, const D3DDISPLAYMODEFILTER* f, UINT m, D3DDISPLAYMODEEX* pm) override { return m_real->EnumAdapterModesEx(a, f, m, pm); }
    HRESULT WINAPI GetAdapterDisplayModeEx(UINT a, D3DDISPLAYMODEEX* pm, D3DDISPLAYROTATION* r) override { return m_real->GetAdapterDisplayModeEx(a, pm, r); }
    HRESULT WINAPI CreateDeviceEx(UINT a, D3DDEVTYPE dt, HWND h, DWORD bf, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* fs, IDirect3DDevice9Ex** ppDev) override { return m_real->CreateDeviceEx(a, dt, h, bf, pp, fs, ppDev); }
    HRESULT WINAPI GetAdapterLUID(UINT a, LUID* luid) override { return m_real->GetAdapterLUID(a, luid); }
};

// ============================================================================
//  Exported entry points
// ============================================================================
extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    if (!ensure_init()) return nullptr;

    // Preferred: create an Ex factory and hand back the upgrading wrapper.
    if (p_Create9Ex != nullptr) {
        IDirect3D9Ex* ex = nullptr;
        if (SUCCEEDED(p_Create9Ex(SDKVersion, &ex)) && ex != nullptr)
            return new ProxyD3D9Ex(ex);
    }
    // Fallback: behave exactly like the system DLL.
    return p_Create9 ? p_Create9(SDKVersion) : nullptr;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
    if (!ensure_init() || p_Create9Ex == nullptr) return D3DERR_NOTAVAILABLE;
    // Caller already wants Ex — pass straight through (already supports CreateDeviceEx).
    return p_Create9Ex(SDKVersion, ppD3D);
}

// ── D3DPERF_* profiling exports: forward to the system DLL ──────────────────────
int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name)
{
    if (!ensure_init()) return -1;
    static PFN_BeginEvent f = reinterpret_cast<PFN_BeginEvent>(GetProcAddress(g_sys_d3d9, "D3DPERF_BeginEvent"));
    return f ? f(col, name) : -1;
}
int WINAPI D3DPERF_EndEvent(void)
{
    if (!ensure_init()) return -1;
    static PFN_EndEvent f = reinterpret_cast<PFN_EndEvent>(GetProcAddress(g_sys_d3d9, "D3DPERF_EndEvent"));
    return f ? f() : -1;
}
void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name)
{
    if (!ensure_init()) return;
    static PFN_SetMarker f = reinterpret_cast<PFN_SetMarker>(GetProcAddress(g_sys_d3d9, "D3DPERF_SetMarker"));
    if (f) f(col, name);
}
void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name)
{
    if (!ensure_init()) return;
    static PFN_SetRegion f = reinterpret_cast<PFN_SetRegion>(GetProcAddress(g_sys_d3d9, "D3DPERF_SetRegion"));
    if (f) f(col, name);
}
BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    if (!ensure_init()) return FALSE;
    static PFN_QueryRepeatFrame f = reinterpret_cast<PFN_QueryRepeatFrame>(GetProcAddress(g_sys_d3d9, "D3DPERF_QueryRepeatFrame"));
    return f ? f() : FALSE;
}
void WINAPI D3DPERF_SetOptions(DWORD opts)
{
    if (!ensure_init()) return;
    static PFN_SetOptions f = reinterpret_cast<PFN_SetOptions>(GetProcAddress(g_sys_d3d9, "D3DPERF_SetOptions"));
    if (f) f(opts);
}
DWORD WINAPI D3DPERF_GetStatus(void)
{
    if (!ensure_init()) return 0;
    static PFN_GetStatus f = reinterpret_cast<PFN_GetStatus>(GetProcAddress(g_sys_d3d9, "D3DPERF_GetStatus"));
    return f ? f() : 0;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        // Defer all loading to first use (LoadLibrary in DllMain risks the loader lock).
    }
    return TRUE;
}
