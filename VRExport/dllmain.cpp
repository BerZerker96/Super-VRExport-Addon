#include "pch.h"
#include "reshade.hpp"
#include <dxgi1_6.h>
#include <d3d9.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d11_1.h>   // ID3D11Device1 for OpenSharedResource1
#include <d3d12.h>
#include <cstdio>    // snprintf
#include <cstdint>   // uint32_t etc.

// Vulkan support is optional — only compiled when the Vulkan SDK is present.
// build.bat passes /DSUPVR_VULKAN=1 when VULKAN_SDK env var is set.
#if SUPVR_VULKAN
#  define VK_USE_PLATFORM_WIN32_KHR
#  include <vulkan/vulkan.h>
#else
// Minimal stubs so the file compiles without the Vulkan SDK installed.
typedef void*    VkDevice;
typedef uint64_t VkImage;
typedef uint64_t VkDeviceMemory;
typedef uint32_t VkFormat;
typedef int      VkResult;
#define VK_NULL_HANDLE 0u
#define VK_SUCCESS 0
#define VKAPI_PTR __stdcall
typedef struct { uint64_t size; uint32_t memoryTypeBits; uint32_t alignment; } VkMemoryRequirements;
typedef struct { int sType; void* pNext; uint64_t allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;
typedef struct { uint32_t width, height, depth; } VkExtent3D;
typedef struct { int sType; void* pNext; uint32_t handleTypes; } VkExternalMemoryImageCreateInfo;
typedef struct { int sType; void* pNext; int imageType; VkFormat format; VkExtent3D extent;
                 uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
                 uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; int initialLayout; } VkImageCreateInfo;
typedef struct { int sType; void* pNext; uint32_t handleType; HANDLE handle; LPCWSTR name; } VkImportMemoryWin32HandleInfoKHR;
typedef VkResult(VKAPI_PTR* PFN_vkGetDeviceProcAddr)(VkDevice, const char*);
typedef VkResult(VKAPI_PTR* PFN_vkCreateImage)(VkDevice, const VkImageCreateInfo*, void*, VkImage*);
typedef void    (VKAPI_PTR* PFN_vkDestroyImage)(VkDevice, VkImage, void*);
typedef void    (VKAPI_PTR* PFN_vkGetImageMemoryRequirements)(VkDevice, VkImage, VkMemoryRequirements*);
typedef VkResult(VKAPI_PTR* PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, void*, VkDeviceMemory*);
typedef void    (VKAPI_PTR* PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, void*);
typedef VkResult(VKAPI_PTR* PFN_vkBindImageMemory)(VkDevice, VkImage, VkDeviceMemory, uint64_t);
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO                      14
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO                    5
#define VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO 1000072001
#define VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR 1000073000
#define VK_IMAGE_TYPE_2D                             1
#define VK_SAMPLE_COUNT_1_BIT                        1
#define VK_IMAGE_TILING_OPTIMAL                      1
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT              2
#define VK_IMAGE_USAGE_SAMPLED_BIT                   4
#define VK_SHARING_MODE_EXCLUSIVE                    0
#define VK_IMAGE_LAYOUT_UNDEFINED                    0
// VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT (0x800) = NT handle — not used.
// We use D3D11_TEXTURE_KMT_BIT (0x400) via VK_EXT_MEM_KMT defined in share_vulkan.
#define VK_FORMAT_R8G8B8A8_UNORM                    37
#define VK_FORMAT_B8G8R8A8_UNORM                    44
#define VK_FORMAT_A2B10G10R10_UNORM_PACK32          64
#define VK_FORMAT_R16G16B16A16_SFLOAT               97
#define VK_FORMAT_R16G16B16A16_UNORM                91
#define VK_FORMAT_B10G11R11_UFLOAT_PACK32          122
#define VK_FORMAT_R32G32B32A32_SFLOAT              109
#define VK_EXT_MEM_KMT                    0x00000400u // D3D11 KMT handle type for Vulkan import
#endif
#include <GL/gl.h>

// Logging: call ReShadeLogMessage via GetProcAddress — compatible with all header versions.
// Levels: 1=error, 2=warning, 3=info — matches ReShade convention.
namespace supvr_log {
    inline HMODULE find_reshade_module() {
        // ReShade can be loaded as dxgi.dll, d3d11.dll, d3d12.dll, opengl32.dll,
        // ReShade64.dll, or ReShade32.dll depending on deployment mode.
        // Enumerate loaded modules to find the one exporting ReShadeLogMessage.
        static HMODULE cached = nullptr;
        if (cached) return cached;
        // Try common names first.
        const char* names[] = {
            "ReShade64.dll", "ReShade32.dll",
            "dxgi.dll", "d3d11.dll", "d3d12.dll", "opengl32.dll", nullptr
        };
        for (int i = 0; names[i]; ++i) {
            HMODULE h = GetModuleHandleA(names[i]);
            if (h && GetProcAddress(h, "ReShadeLogMessage")) { cached = h; return h; }
        }
        return nullptr;
    }
    inline void write(int level, const char* msg) {
        static const auto fn = reinterpret_cast<void(*)(HMODULE, int, const char*)>(
            GetProcAddress(find_reshade_module(), "ReShadeLogMessage"));
        if (fn) fn(nullptr, level, msg);
    }
}
#define LOG_ERR(msg) supvr_log::write(1, msg)
#define LOG_INF(msg) supvr_log::write(3, msg)

extern "C" __declspec(dllexport) const char* NAME        = "SuperVrExport";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Export SuperDepth3D full-res stereo buffers to KatangaVR and VRScreenCap.";

// ── WGL DX interop (NVIDIA legacy OpenGL path) ───────────────────────────────
typedef HANDLE   (WINAPI* PFN_wglDXOpenDeviceNV)      (void*);
typedef BOOL     (WINAPI* PFN_wglDXCloseDeviceNV)     (HANDLE);
typedef HANDLE   (WINAPI* PFN_wglDXRegisterObjectNV)  (HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL     (WINAPI* PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL     (WINAPI* PFN_wglDXLockObjectsNV)     (HANDLE, GLint, HANDLE*);
typedef BOOL     (WINAPI* PFN_wglDXUnlockObjectsNV)   (HANDLE, GLint, HANDLE*);
static PFN_wglDXOpenDeviceNV       s_wglDXOpenDeviceNV       = nullptr;
static PFN_wglDXCloseDeviceNV      s_wglDXCloseDeviceNV      = nullptr;
static PFN_wglDXRegisterObjectNV   s_wglDXRegisterObjectNV   = nullptr;
static PFN_wglDXUnregisterObjectNV s_wglDXUnregisterObjectNV = nullptr;
static PFN_wglDXLockObjectsNV      s_wglDXLockObjectsNV      = nullptr;
static PFN_wglDXUnlockObjectsNV    s_wglDXUnlockObjectsNV    = nullptr;

// GL extended types not in GL/gl.h on Windows (only GL 1.1 is in the system header).
typedef unsigned long long GLuint64;
#ifndef GL_SIZEIPTR_TYPE
typedef SIZE_T GLsizeiptr;
#endif

// ── GL_EXT_memory_object_win32 (cross-vendor OpenGL path) ────────────────────
typedef void (APIENTRY* PFN_glCreateMemoryObjectsEXT)    (GLsizei, GLuint*);
typedef void (APIENTRY* PFN_glDeleteMemoryObjectsEXT)    (GLsizei, const GLuint*);
typedef void (APIENTRY* PFN_glImportMemoryWin32HandleEXT)(GLuint, GLuint64, GLenum, void*);
typedef void (APIENTRY* PFN_glTextureStorageMem2DEXT)    (GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
static PFN_glCreateMemoryObjectsEXT     s_glCreateMemoryObjectsEXT     = nullptr;
static PFN_glDeleteMemoryObjectsEXT     s_glDeleteMemoryObjectsEXT     = nullptr;
static PFN_glImportMemoryWin32HandleEXT s_glImportMemoryWin32HandleEXT = nullptr;
static PFN_glTextureStorageMem2DEXT     s_glTextureStorageMem2DEXT     = nullptr;

// ── PBO functions (OpenGL 1.5+) ───────────────────────────────────────────────
typedef void      (APIENTRY* PFN_glGenBuffers)   (GLsizei, GLuint*);
typedef void      (APIENTRY* PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void      (APIENTRY* PFN_glBindBuffer)   (GLenum, GLuint);
typedef void      (APIENTRY* PFN_glBufferData)   (GLenum, GLsizeiptr, const void*, GLenum);
typedef void*     (APIENTRY* PFN_glMapBuffer)    (GLenum, GLenum);
typedef GLboolean (APIENTRY* PFN_glUnmapBuffer)  (GLenum);
static PFN_glGenBuffers    s_glGenBuffers    = nullptr;
static PFN_glDeleteBuffers s_glDeleteBuffers = nullptr;
static PFN_glBindBuffer    s_glBindBuffer    = nullptr;
static PFN_glBufferData    s_glBufferData    = nullptr;
static PFN_glMapBuffer     s_glMapBuffer     = nullptr;
static PFN_glUnmapBuffer   s_glUnmapBuffer   = nullptr;

// GL constants
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER            0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ                  0x88E1
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY                    0x88B8
#endif
#ifndef GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT
#define GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT 0x958C
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2 0x8059
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

// ── Vulkan function pointers ──────────────────────────────────────────────────
static PFN_vkCreateImage                s_vkCreateImage                = nullptr;
static PFN_vkDestroyImage               s_vkDestroyImage               = nullptr;
static PFN_vkGetImageMemoryRequirements s_vkGetImageMemoryRequirements = nullptr;
static PFN_vkAllocateMemory             s_vkAllocateMemory             = nullptr;
static PFN_vkFreeMemory                 s_vkFreeMemory                 = nullptr;
static PFN_vkBindImageMemory            s_vkBindImageMemory            = nullptr;
static VkDevice       g_vk_device        = nullptr;
static VkImage        g_vk_image         = VK_NULL_HANDLE;
static VkDeviceMemory g_vk_memory        = VK_NULL_HANDLE;
static uint32_t       g_vk_width         = 0;
static uint32_t       g_vk_height        = 0;

// ── Shared texture state ──────────────────────────────────────────────────────
static IDXGIKeyedMutex* sharedTextureMutex  = nullptr;
static void*            sharedTexture       = nullptr;  // API-specific copy dst
static void*            sharedTexture_D3D11 = nullptr;  // always D3D11 tex for KatanaVR
// FIX: single persistent file mapping handle (no leak on repeated calls)
static HANDLE           g_katanga_mapping   = nullptr;
static HANDLE*          g_katanga_view      = nullptr;

// OpenGL interop handles
static HANDLE  g_wgl_device  = nullptr;
static HANDLE  g_wgl_object  = nullptr;
static GLuint  g_gl_mem_obj  = 0;
// FIX: track whether GL EXT path is active (no copy needed — shared memory)
static bool    g_gl_ext_active = false;

// PBO ping-pong for CPU fallback
static GLuint   g_pbo[2]    = { 0, 0 };
static uint32_t g_pbo_frame = 0;
static uint32_t g_pbo_w     = 0;
static uint32_t g_pbo_h     = 0;

// Standalone D3D11 device for cross-API bridging
static ID3D11Device*        g_d3d11_device  = nullptr;
static ID3D11DeviceContext* g_d3d11_context = nullptr;
// Thread safety: export_effects (reload thread) and add_copy_command (render thread)
// both touch shared state. Guard with a CRITICAL_SECTION.
static CRITICAL_SECTION g_cs;
static bool             g_cs_init = false;
inline void cs_enter() { if (g_cs_init) EnterCriticalSection(&g_cs); }
inline void cs_leave() { if (g_cs_init) LeaveCriticalSection(&g_cs); }

// FIX: cache the texture variable — avoid 5x string lookups per frame
static reshade::api::effect_texture_variable g_cached_vr_tex = { 0 };
static bool g_tex_cache_dirty = true; // set on reload; render thread re-searches if true
// Cache source texture dimensions and format to detect changes
static uint32_t             g_src_width  = 0;
static uint32_t             g_src_height = 0;
// Sentinel value: 0xFFFFFFFF cannot be a valid ReShade format enum value
#define SRC_FORMAT_UNSET static_cast<reshade::api::format>(0xFFFFFFFFu)
static reshade::api::format g_src_format = SRC_FORMAT_UNSET;

// ── FA / uniform state ────────────────────────────────────────────────────────
static reshade::api::effect_runtime*         g_active_runtime     = nullptr;
static bool     g_reload_attempted = false;
static uint32_t g_reload_count     = 0;   // total reloads fired this session
static bool     g_vr_ready           = false; // true once DoubleTex + FA found successfully
static reshade::api::device_api  g_cached_api = reshade::api::device_api::d3d12; // cached API
static uint32_t                  g_acquiresync_fail_count = 0; // consecutive AcquireSync failures

// ── Format helpers ────────────────────────────────────────────────────────────

static DXGI_FORMAT reshade_to_dxgi(reshade::api::format fmt)
{
    // Values verified against reshade_api_format.hpp enum
    switch (static_cast<uint32_t>(fmt)) {
    case 28:  return DXGI_FORMAT_R8G8B8A8_UNORM;       // r8g8b8a8_unorm
    case 29:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // r8g8b8a8_unorm_srgb
    case 87:  return DXGI_FORMAT_B8G8R8A8_UNORM;        // b8g8r8a8_unorm
    case 91:  return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;  // b8g8r8a8_unorm_srgb
    case 24:  return DXGI_FORMAT_R10G10B10A2_UNORM;     // r10g10b10a2_unorm
    case 10:  return DXGI_FORMAT_R16G16B16A16_FLOAT;    // r16g16b16a16_float
    case 11:  return DXGI_FORMAT_R16G16B16A16_UNORM;    // r16g16b16a16_unorm
    case 26:  return DXGI_FORMAT_R11G11B10_FLOAT;       // r11g11b10_float
    case 2:   return DXGI_FORMAT_R32G32B32A32_FLOAT;    // r32g32b32a32_float
    default:  return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// FIX: resolve typeless DXGI formats to typed equivalents for shared resources
static DXGI_FORMAT dxgi_ensure_typed(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return fmt;
    }
}

// FIX: map DXGI format to matching VkFormat
static VkFormat dxgi_to_vkformat(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:   return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM:   return VK_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT:      return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:   return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:                                return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

// FIX: map DXGI format to GL internal format and transfer format/type
struct GLFormat { GLenum internal, format, type; };
static GLFormat dxgi_to_gl(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return { GL_RGBA8,    0x80E1 /*GL_BGRA*/,  GL_UNSIGNED_BYTE };
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return { GL_RGB10_A2, GL_RGBA,              0x8368 /*GL_UNSIGNED_INT_2_10_10_10_REV*/ };
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return { GL_RGBA16F,  GL_RGBA,              0x140B /*GL_HALF_FLOAT*/ };
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return { 0x8056 /*GL_RGBA16*/, GL_RGBA,     GL_UNSIGNED_SHORT };
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return { 0x8C3A /*GL_R11F_G11F_B10F*/, 0x8C6F /*GL_RGB*/, 0x8D9F /*GL_UNSIGNED_INT_10F_11F_11F_REV*/ };
    default: // RGBA8
        return { GL_RGBA8,    GL_RGBA,              GL_UNSIGNED_BYTE };
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static reshade::api::effect_uniform_variable find_sd3d_uniform(
    reshade::api::effect_runtime* rt, const char* name)
{
    char buf[128];
    // Use nullptr effect name to search all effects — avoids path mismatch issues.
    snprintf(buf, sizeof(buf), "V__SuperDepth3D__%s", name);
    auto h = rt->find_uniform_variable(nullptr, buf);
    if (h.handle == 0) {
        snprintf(buf, sizeof(buf), "V__%s", name);
        h = rt->find_uniform_variable(nullptr, buf);
    }
    return h;
}

// Uniform handles — set after each compile.
static reshade::api::effect_uniform_variable g_fs_fa_handle      = { 0 };
static reshade::api::effect_uniform_variable g_stereo_mode_handle = { 0 };

static void apply_fa_state(reshade::api::effect_runtime* rt, bool on)
{
    if (!on) {
        // Revert to SBS, FS_FA off
        if (g_stereo_mode_handle.handle) { int32_t m=0; rt->set_uniform_value_int(g_stereo_mode_handle,&m,1); }
        if (g_fs_fa_handle.handle)       { bool f=false; rt->set_uniform_value_bool(g_fs_fa_handle,&f,1); }
        return;
    }
    if (g_fs_fa_handle.handle) {
        // Non-VR + EX_DLP_FS_Mode: set Frame Sequential (mode 6) + FS_FA=true
        // Feeds FS output into 3DToElse which assembles full SBS for KatanaVR.
        if (g_stereo_mode_handle.handle) { int32_t m=6; rt->set_uniform_value_int(g_stereo_mode_handle,&m,1); }
        bool fa=true; rt->set_uniform_value_bool(g_fs_fa_handle, &fa, 1);
    } else if (g_stereo_mode_handle.handle) {
        // VR mode or DoubleTex-only: ensure SBS (mode 0)
        int32_t m=0; rt->set_uniform_value_int(g_stereo_mode_handle, &m, 1);
    }
}

static bool ensure_d3d11_device(IDXGIAdapter* adapter = nullptr)
{
    if (g_d3d11_device) return true;
    // Load d3d11.dll directly to bypass ReShade's D3D11CreateDevice hook.
    // If we go through the hook, ReShade wraps our device in a proxy which
    // breaks cross-API sharing (OpenSharedResource1, CreateSharedHandle etc).
    typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT,
        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HMODULE d3d11_dll = GetModuleHandleA("d3d11.dll");
    if (!d3d11_dll) d3d11_dll = LoadLibraryA("d3d11.dll");
    if (!d3d11_dll) return false;
    // Get the REAL D3D11CreateDevice by finding it via the DLL export table.
    // ReShade replaces the call in the IAT but GetProcAddress returns the real one
    // if called on the system DLL handle (before ReShade hooks it at the IAT level).
    // On some setups this may still be hooked — in that case the proxy is unavoidable,
    // but the shared resource ops should still work since ReShade passes through
    // resource creation calls on its proxy devices.
    auto pfn = reinterpret_cast<PFN_D3D11CreateDevice>(
        GetProcAddress(d3d11_dll, "D3D11CreateDevice"));
    if (!pfn) return false;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D_DRIVER_TYPE dtype = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    if (FAILED(pfn(adapter, dtype, nullptr,
        0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return false;
    // Only assign if still unset (another thread may have raced us).
    if (InterlockedCompareExchangePointer(
            reinterpret_cast<volatile PVOID*>(&g_d3d11_device), dev, nullptr) != nullptr) {
        // Another thread won — release our duplicate.
        ctx->Release(); dev->Release();
    } else {
        // Assign context under CS so readers see device+context together.
        cs_enter();
        g_d3d11_context = ctx;
        cs_leave();
    }
    return g_d3d11_device != nullptr;
}

// FIX: persistent file mapping — create once, update in-place
static void write_katanga_handle(HANDLE h)
{
    // Recreate mapping every call so KatanaVR detects the new handle.
    // KatanaVR opens KatangaMappedFile by name — closing and recreating
    // signals it to re-open and pick up the new D3D resource handle.
    if (g_katanga_view)    { UnmapViewOfFile(g_katanga_view);  g_katanga_view    = nullptr; }
    if (g_katanga_mapping) { CloseHandle(g_katanga_mapping);   g_katanga_mapping = nullptr; }
    g_katanga_mapping = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(h), L"Local\\KatangaMappedFile");
    if (!g_katanga_mapping) return;
    g_katanga_view = reinterpret_cast<HANDLE*>(MapViewOfFile(
        g_katanga_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(h)));
    if (g_katanga_view) *g_katanga_view = h;
}

// Forward declaration — defined and initialised here (used in release_shared above share_d3d9).
static IDirect3DTexture9* g_d3d9_shared_tex = nullptr;

// FIX: unified cleanup — releases sharedTexture and sharedTexture_D3D11 safely
static void release_shared(reshade::api::device_api api)
{
    if (sharedTextureMutex) { sharedTextureMutex->Release(); sharedTextureMutex = nullptr; }
    // Release the API-specific copy dst
    if (sharedTexture && sharedTexture != sharedTexture_D3D11) {
        switch (api) {
        case reshade::api::device_api::d3d11:
            static_cast<ID3D11Texture2D*>(sharedTexture)->Release(); break;
        case reshade::api::device_api::d3d12:
            static_cast<ID3D12Resource*>(sharedTexture)->Release(); break;
        case reshade::api::device_api::d3d10:
            static_cast<ID3D10Texture2D*>(sharedTexture)->Release(); break;
        case reshade::api::device_api::d3d9:
            // sharedTexture = IDirect3DSurface9* (level 0 of g_d3d9_shared_tex)
            // Release the surface ref from GetSurfaceLevel, then the texture.
            static_cast<IDirect3DSurface9*>(sharedTexture)->Release();
            if (g_d3d9_shared_tex) { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = nullptr; }
            break;
        default: break;
        }
        sharedTexture = nullptr;
    }
    // Release the D3D11 shared texture (KatanaVR-facing)
    if (sharedTexture_D3D11) {
        static_cast<ID3D11Texture2D*>(sharedTexture_D3D11)->Release();
        sharedTexture_D3D11 = nullptr;
    }
    sharedTexture = nullptr;
    g_acquiresync_fail_count = 0;
}

// Create D3D11 shared texture: gets legacy handle for KatanaVR + NT handle for GPU import
static ID3D11Texture2D* create_d3d11_shared(
    uint32_t w, uint32_t h, DXGI_FORMAT fmt, HANDLE* out_nt,
    IDXGIAdapter* adapter = nullptr)
{
    if (!ensure_d3d11_device(adapter)) return nullptr;
    fmt = dxgi_ensure_typed(fmt);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt; d.SampleDesc = {1,0}; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // SHARED only: gives a legacy KMT handle via GetSharedHandle.
    // KMT handle is used for BOTH KatangaMappedFile (VRScreenCap D3D11 path)
    // AND D3D12 OpenSharedHandle (game device import). D3D12 accepts KMT handles.
    // No NTHANDLE or KEYED_MUTEX needed — simpler and more compatible.
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_d3d11_device->CreateTexture2D(&d, nullptr, &tex))) return nullptr;

    // Get KMT handle — works with SHARED flag, used for both KatangaMappedFile
    // (VRScreenCap D3D11 path) and D3D12 OpenSharedHandle (game device import).
    IDXGIResource* r = nullptr; HANDLE leg = nullptr;
    if (SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
    if (leg) {
        write_katanga_handle(leg);
        if (out_nt) *out_nt = leg; // KMT handle used as D3D12 import handle too
    } else {
        LOG_ERR("SuperVrExport: create_d3d11_shared: GetSharedHandle failed");
        if (out_nt) *out_nt = nullptr;
    }

    // No IDXGIKeyedMutex (incompatible with SHARED flag).
    // Callers call release_shared before create_d3d11_shared, so these are normally
    // already null. The checks here guard against direct calls (e.g. Vulkan/OpenGL paths).
    if (sharedTextureMutex)  { sharedTextureMutex->Release();  sharedTextureMutex  = nullptr; }
    if (sharedTexture_D3D11) { static_cast<ID3D11Texture2D*>(sharedTexture_D3D11)->Release(); sharedTexture_D3D11 = nullptr; }
    sharedTextureMutex  = nullptr; // no keyed mutex with SHARED flag
    sharedTexture_D3D11 = tex;
    return tex;
}

// Open shared handle on standalone D3D11 device (D3D9/D3D12 bridge path)
static bool open_on_d3d11(HANDLE h, bool is_nt)
{
    if (!ensure_d3d11_device()) return false;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr;
    if (is_nt) {
        // OpenSharedResource1 is on ID3D11Device1 (requires Win8+ / D3D11.1).
        ID3D11Device1* dev1 = nullptr;
        if (SUCCEEDED(g_d3d11_device->QueryInterface(IID_PPV_ARGS(&dev1)))) {
            hr = dev1->OpenSharedResource1(h, IID_PPV_ARGS(&tex));
            dev1->Release();
        } else {
            // Fallback: open without NT handle type (legacy path)
            hr = g_d3d11_device->OpenSharedResource(h, IID_PPV_ARGS(&tex));
        }
    } else {
        hr = g_d3d11_device->OpenSharedResource(h, IID_PPV_ARGS(&tex));
    }
    if (is_nt) CloseHandle(h);
    if (FAILED(hr)) return false;

    IDXGIKeyedMutex* mutex = nullptr;
    tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex));
    IDXGIResource* r = nullptr; HANDLE leg = nullptr;
    if (SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
    if (!leg) { tex->Release(); if (mutex) mutex->Release(); return false; }

    write_katanga_handle(leg);
    sharedTextureMutex  = mutex;
    sharedTexture_D3D11 = tex;
    sharedTexture       = tex;
    return true;
}

// ── Per-API share functions ───────────────────────────────────────────────────

static void share_d3d11(ID3D11Texture2D* src, ID3D11Device* dev,
                         reshade::api::device_api api)
{
    release_shared(api);

    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    d.BindFlags    = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.Usage        = D3D11_USAGE_DEFAULT; // source may be DYNAMIC; shared tex must be DEFAULT
    d.MiscFlags    = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    d.Format = dxgi_ensure_typed(d.Format);

    // Unity: KEYEDMUTEX on game device crashes. Use standalone bridge.
    if (GetModuleHandleA("UnityPlayer.dll")) {
        HANDLE kmt = nullptr;
        ID3D11Texture2D* standalone_tex = create_d3d11_shared(d.Width, d.Height, d.Format, &kmt);
        if (!standalone_tex || !kmt) { LOG_ERR("SuperVrExport: Unity bridge: setup failed"); return; }
        ID3D11Texture2D* game_side = nullptr;
        if (FAILED(dev->OpenSharedResource(kmt, IID_PPV_ARGS(&game_side)))) {
            LOG_ERR("SuperVrExport: Unity bridge: OpenSharedResource failed"); return;
        }
        sharedTexture = game_side;
        LOG_INF("SuperVrExport: D3D11 ready (Unity standalone bridge)");
        return;
    }
    ID3D11Texture2D* shared = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &shared))) {
        LOG_ERR("SuperVrExport: D3D11 CreateTexture2D failed"); return;
    }
    IDXGIKeyedMutex* mutex = nullptr;
    shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex));
    IDXGIResource* r = nullptr; HANDLE leg = nullptr;
    if (SUCCEEDED(shared->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
    if (!leg) { shared->Release(); if (mutex) mutex->Release(); LOG_ERR("SuperVrExport: D3D11 GetSharedHandle failed"); return; }

    write_katanga_handle(leg);
    sharedTextureMutex  = mutex;
    sharedTexture_D3D11 = shared;
    sharedTexture       = shared;
    LOG_INF("SuperVrExport: D3D11 ready");
}

static void share_d3d10(ID3D10Texture2D* src, ID3D10Device* dev)
{
    release_shared(reshade::api::device_api::d3d10);
    D3D10_TEXTURE2D_DESC d; src->GetDesc(&d);
    d.BindFlags    = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.Usage        = D3D10_USAGE_DEFAULT;
    d.MiscFlags    = D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ID3D10Texture2D* shared = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &shared))) {
        LOG_ERR("SuperVrExport: D3D10 CreateTexture2D failed"); return;
    }
    IDXGIKeyedMutex* mutex = nullptr;
    shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex));
    IDXGIResource* r = nullptr; HANDLE leg = nullptr;
    if (SUCCEEDED(shared->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
    if (!leg) { shared->Release(); if (mutex) mutex->Release(); LOG_ERR("SuperVrExport: D3D10 GetSharedHandle failed"); return; }

    write_katanga_handle(leg);
    sharedTextureMutex  = mutex;
    sharedTexture_D3D11 = nullptr; // D3D10 tex stored separately
    sharedTexture       = shared;
    LOG_INF("SuperVrExport: D3D10 ready");
}

static void share_d3d9(IDirect3DTexture9* src, IDirect3DDevice9* dev)
{
    // Release previous D3D9 shared tex
    if (g_d3d9_shared_tex) { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = nullptr; }
    release_shared(reshade::api::device_api::d3d9);

    IDirect3DDevice9Ex* devEx = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&devEx)))) {
        LOG_ERR("SuperVrExport: D3D9 needs IDirect3DDevice9Ex"); return;
    }
    D3DSURFACE_DESC d; src->GetLevelDesc(0, &d);
    IDirect3DTexture9* s9 = nullptr; HANDLE h9 = nullptr;
    HRESULT hr = devEx->CreateTexture(d.Width, d.Height, 1,
        D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &s9, &h9);
    devEx->Release();
    if (FAILED(hr) || !h9) { if (s9) s9->Release(); LOG_ERR("SuperVrExport: D3D9 CreateTexture failed"); return; }

    // Open s9 on D3D11 for KatanaVR — same GPU memory.
    if (!open_on_d3d11(h9, false)) {
        s9->Release();
        LOG_ERR("SuperVrExport: D3D9 bridge failed"); return;
    }
    // Get surface level 0 — ReShade D3D9 resource handles are IDirect3DSurface9* not IDirect3DTexture9*.
    IDirect3DSurface9* surf = nullptr;
    s9->GetSurfaceLevel(0, &surf);
    g_d3d9_shared_tex = s9; // holds ref to keep texture alive
    sharedTexture = surf;   // copy_resource dst = surface level 0 (same-API copy, valid)
    LOG_INF("SuperVrExport: D3D9 ready");
}

static void share_d3d12(ID3D12Resource* src, ID3D12Device* dev)
{
    release_shared(reshade::api::device_api::d3d12);
    // Reversed bridge: create D3D11 shared tex → open as D3D12 copy dst on the GAME device.
    // This avoids D3D12_HEAP_FLAG_SHARED which RE Engine (and many D3D12 games) block entirely.
    // VRScreenCap reads via its D3D11 path using the legacy KMT handle we write to KatangaMappedFile.
    if (!ensure_d3d11_device()) { LOG_ERR("SuperVrExport: D3D12 bridge: no D3D11 device"); return; }

    D3D12_RESOURCE_DESC rd = src->GetDesc();
    DXGI_FORMAT src_fmt = dxgi_ensure_typed(rd.Format);

    // Create D3D11 shared texture in the SAME format as the source.
    // D3D11 supports sharing R10G10B10A2_UNORM and most other formats without issue.
    // Using the native format ensures copy_resource(D3D12_src → D3D12_from_D3D11_dst)
    // is a same-format copy — valid and produces correct colours in VRScreenCap.
    // Use game D3D12 device's adapter for our standalone D3D11 device.
    // Cross-adapter shared handles (e.g. integrated vs discrete GPU) produce black frames.
    IDXGIDevice* dxgi_dev12 = nullptr;
    IDXGIAdapter* game_adapter = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev12)))) {
        dxgi_dev12->GetAdapter(&game_adapter); dxgi_dev12->Release();
    }
    HANDLE nt_for_d3d12 = nullptr;
    ID3D11Texture2D* d11 = create_d3d11_shared((UINT)rd.Width, (UINT)rd.Height,
        src_fmt, &nt_for_d3d12, game_adapter);
    if (game_adapter) { game_adapter->Release(); game_adapter = nullptr; }
    if (!d11) { LOG_ERR("SuperVrExport: D3D12 bridge: create_d3d11_shared failed"); return; }
    if (!nt_for_d3d12) { LOG_ERR("SuperVrExport: D3D12 bridge: KMT handle from D3D11 failed"); return; }
    LOG_INF("SuperVrExport: D3D12 bridge: got KMT handle, opening on game D3D12 device");

    // Open the D3D11 shared texture on the game D3D12 device using the KMT handle.
    // D3D12::OpenSharedHandle accepts KMT handles from D3D11 SHARED textures.
    // DO NOT CloseHandle on KMT handles — they are not reference-counted like NT handles.
    ID3D12Resource* d12 = nullptr;
    if (FAILED(dev->OpenSharedHandle(nt_for_d3d12, IID_PPV_ARGS(&d12)))) {
        LOG_ERR("SuperVrExport: D3D12 bridge: OpenSharedHandle on game D3D12 device failed");
        return;
    }

    // KatangaMappedFile already written by create_d3d11_shared above.

    // sharedTexture = D3D12 resource (game device side) used as copy_resource dst.
    // sharedTexture_D3D11 and sharedTextureMutex set by create_d3d11_shared (KatanaVR-facing).
    sharedTexture = d12;

    // Formats always match (D3D11 tex created in src_fmt) — no mismatch possible.
    char rdy[128]; snprintf(rdy, sizeof(rdy),
        "SuperVrExport: D3D12 ready (D3D11 bridge, fmt=0x%X, KMT handle to VRScreenCap)", (unsigned)src_fmt);
    LOG_INF(rdy);
}

static void share_vulkan(reshade::api::resource src_res, reshade::api::device* dev)
{
    auto desc = dev->get_resource_desc(src_res);
    uint32_t w = desc.texture.width, h = desc.texture.height;
    DXGI_FORMAT dxgi_fmt = reshade_to_dxgi(desc.texture.format);
    VkFormat vk_fmt = dxgi_to_vkformat(dxgi_fmt);

    VkDevice vk_dev = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(dev->get_native()));
    // Re-create if resolution changed (uses old g_vk_device for cleanup, then updates).
    if (g_vk_image != VK_NULL_HANDLE && (g_vk_width != w || g_vk_height != h)) {
        if (s_vkDestroyImage) s_vkDestroyImage(g_vk_device, g_vk_image, nullptr);
        if (s_vkFreeMemory)   s_vkFreeMemory  (g_vk_device, g_vk_memory, nullptr);
        g_vk_image = VK_NULL_HANDLE; g_vk_memory = VK_NULL_HANDLE;
        release_shared(reshade::api::device_api::vulkan);
    }
    if (g_vk_image != VK_NULL_HANDLE) return; // already set up at same resolution
    g_vk_device = vk_dev; // update device after confirming we will proceed

    HMODULE vk_dll = GetModuleHandleA("vulkan-1.dll");
    if (!vk_dll) return;
    auto vkGetDevProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(vk_dll, "vkGetDeviceProcAddr"));
    if (!vkGetDevProc) return;

    // Only load VK function pointers on first setup or if device changed.
    if (!s_vkCreateImage) {
#define LOAD_VK(fn) s_##fn = reinterpret_cast<PFN_##fn>(vkGetDevProc(vk_dev, #fn)); \
                        if (!s_##fn) { LOG_ERR("SuperVrExport: Vulkan missing " #fn); return; }
        LOAD_VK(vkCreateImage) LOAD_VK(vkDestroyImage)
        LOAD_VK(vkGetImageMemoryRequirements)
        LOAD_VK(vkAllocateMemory) LOAD_VK(vkFreeMemory) LOAD_VK(vkBindImageMemory)
#undef LOAD_VK
    }
    // Create D3D11 shared texture first (we control it), get NT handle for Vulkan import.
    HANDLE nt = nullptr;
    ID3D11Texture2D* d11 = create_d3d11_shared(w, h, dxgi_fmt, &nt);
    if (!d11 || !nt) return;

    VkImportMemoryWin32HandleInfoKHR imp = {};
    imp.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.handleType = VK_EXT_MEM_KMT;
    imp.handle     = nt;

    VkExternalMemoryImageCreateInfo ext = {};
    ext.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXT_MEM_KMT;

    VkImageCreateInfo ic = {};
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ic.pNext = &ext;
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = vk_fmt;
    ic.extent = {w, h, 1}; ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage img = VK_NULL_HANDLE;
    if (s_vkCreateImage(vk_dev, &ic, nullptr, &img) != VK_SUCCESS) {
        return; // nt is a KMT handle — do NOT CloseHandle on KMT handles
    }
    VkMemoryRequirements mr = {};
    s_vkGetImageMemoryRequirements(vk_dev, img, &mr);

    // Iterate valid memory type indices using memoryTypeBits — skips unsupported types.
    // This avoids up to 32 failed vkAllocateMemory calls on each setup.
    VkDeviceMemory mem = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < 32; ++i) {
        if (!((mr.memoryTypeBits >> i) & 1u)) continue; // type not supported for this image
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &imp; ai.allocationSize = mr.size; ai.memoryTypeIndex = i;
        if (s_vkAllocateMemory(vk_dev, &ai, nullptr, &mem) == VK_SUCCESS) break;
        mem = VK_NULL_HANDLE;
    }
    if (!mem) {
        LOG_ERR("SuperVrExport: Vulkan vkAllocateMemory failed all types");
        s_vkDestroyImage(vk_dev, img, nullptr); return; // nt = KMT handle, no CloseHandle
    }
    // nt is a KMT handle from D3D11_RESOURCE_MISC_SHARED — do NOT CloseHandle.

    if (s_vkBindImageMemory(vk_dev, img, mem, 0) != VK_SUCCESS) {
        s_vkFreeMemory(vk_dev, mem, nullptr);
        s_vkDestroyImage(vk_dev, img, nullptr);
        LOG_ERR("SuperVrExport: Vulkan vkBindImageMemory failed"); return;
    }
    g_vk_image = img; g_vk_memory = mem;
    g_vk_width = w;   g_vk_height = h;
    sharedTexture = reinterpret_cast<void*>(static_cast<uintptr_t>(1u)); // sentinel: non-null signals Vulkan is ready; g_vk_image is used for actual copy
    LOG_INF("SuperVrExport: Vulkan ready (D3D11 import bridge)");
}

// ── OpenGL sharing ────────────────────────────────────────────────────────────

static bool load_gl_fns()
{
    if (s_glGenBuffers) return true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
    auto wgl = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(GetProcAddress(gl, "wglGetProcAddress"));
    if (!wgl) return false;
#define W(fn, t) s_##fn = reinterpret_cast<t>(wgl(#fn))
    W(glCreateMemoryObjectsEXT,     PFN_glCreateMemoryObjectsEXT);
    W(glDeleteMemoryObjectsEXT,     PFN_glDeleteMemoryObjectsEXT);
    W(glImportMemoryWin32HandleEXT, PFN_glImportMemoryWin32HandleEXT);
    W(glTextureStorageMem2DEXT,     PFN_glTextureStorageMem2DEXT);
    W(glGenBuffers,    PFN_glGenBuffers);
    W(glDeleteBuffers, PFN_glDeleteBuffers);
    W(glBindBuffer,    PFN_glBindBuffer);
    W(glBufferData,    PFN_glBufferData);
    W(glMapBuffer,     PFN_glMapBuffer);
    W(glUnmapBuffer,   PFN_glUnmapBuffer);
    W(wglDXOpenDeviceNV,       PFN_wglDXOpenDeviceNV);
    W(wglDXCloseDeviceNV,      PFN_wglDXCloseDeviceNV);
    W(wglDXRegisterObjectNV,   PFN_wglDXRegisterObjectNV);
    W(wglDXUnregisterObjectNV, PFN_wglDXUnregisterObjectNV);
    W(wglDXLockObjectsNV,      PFN_wglDXLockObjectsNV);
    W(wglDXUnlockObjectsNV,    PFN_wglDXUnlockObjectsNV);
#undef W
    return s_glGenBuffers != nullptr;
}

static void share_opengl(GLuint gl_tex, reshade::api::resource src_res,
                          reshade::api::device* dev)
{
    // Cleanup previous state
    g_gl_ext_active = false;
    if (g_gl_mem_obj && s_glDeleteMemoryObjectsEXT)          { s_glDeleteMemoryObjectsEXT(1, &g_gl_mem_obj); g_gl_mem_obj = 0; }
    if (g_wgl_object && g_wgl_device) {
        // Unlock first — object may be locked by on_begin_effects_wgl (dim-change path).
        if (s_wglDXUnlockObjectsNV)    s_wglDXUnlockObjectsNV   (g_wgl_device, 1, &g_wgl_object);
        if (s_wglDXUnregisterObjectNV) s_wglDXUnregisterObjectNV(g_wgl_device, g_wgl_object);
        g_wgl_object = nullptr;
    }
    if (g_wgl_device && s_wglDXCloseDeviceNV)                { s_wglDXCloseDeviceNV(g_wgl_device); g_wgl_device = nullptr; }
    release_shared(reshade::api::device_api::opengl);

    load_gl_fns();
    auto desc = dev->get_resource_desc(src_res);
    uint32_t w = desc.texture.width, h = desc.texture.height;
    DXGI_FORMAT fmt = reshade_to_dxgi(desc.texture.format);
    GLFormat glf = dxgi_to_gl(fmt);

    // ── Path 1: GL_EXT_memory_object_win32 (cross-vendor, GPU-direct) ───────
    if (s_glCreateMemoryObjectsEXT && s_glImportMemoryWin32HandleEXT && s_glTextureStorageMem2DEXT) {
        HANDLE nt = nullptr;
        ID3D11Texture2D* d11 = create_d3d11_shared(w, h, fmt, &nt);
        if (d11 && nt) {
            GLuint mem_obj = 0;
            s_glCreateMemoryObjectsEXT(1, &mem_obj);
            // Bytes per pixel — must match actual D3D11 allocation size for driver validation
            size_t bpp;
            switch (fmt) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UNORM: bpp = 8;  break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: bpp = 16; break;
            default:                              bpp = 4;  break;
            }
            GLuint64 mem_sz = (GLuint64)w * h * bpp;
            // create_d3d11_shared uses SHARED flag → KMT handle.
            // Use GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT directly — no NT fallback needed.
            // nt is a KMT handle — do NOT CloseHandle.
            s_glImportMemoryWin32HandleEXT(mem_obj, mem_sz, GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT, nt);
            glGetError(); // clear any import error before checking storage result
            s_glTextureStorageMem2DEXT(gl_tex, 1, glf.internal, (GLsizei)w, (GLsizei)h, mem_obj, 0);
            if (glGetError() == GL_NO_ERROR) {
                g_gl_mem_obj    = mem_obj;
                sharedTexture   = d11;
                g_gl_ext_active = true; // FIX: mark as shared-memory path — no copy needed
                LOG_INF("SuperVrExport: OpenGL via GL_EXT_memory_object_win32");
                return;
            }
            s_glDeleteMemoryObjectsEXT(1, &mem_obj);
            // Fall through to next path
        }
        if (d11) d11->Release();
        // nt = KMT handle — no CloseHandle
    }

    // ── Path 2: WGL_NV_DX_interop2 (NVIDIA legacy, GPU-direct) ─────────────
    if (s_wglDXOpenDeviceNV && ensure_d3d11_device()) {
        g_wgl_device = s_wglDXOpenDeviceNV(g_d3d11_device);
        if (g_wgl_device) {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = dxgi_ensure_typed(fmt); d.SampleDesc = {1,0};
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            ID3D11Texture2D* d11 = nullptr;
            if (SUCCEEDED(g_d3d11_device->CreateTexture2D(&d, nullptr, &d11))) {
                g_wgl_object = s_wglDXRegisterObjectNV(g_wgl_device, d11, gl_tex,
                    GL_TEXTURE_2D, 0x8750 /*WGL_ACCESS_READ_WRITE_NV*/);
                if (g_wgl_object) {
                    IDXGIKeyedMutex* mx = nullptr;
                    d11->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mx));
                    IDXGIResource* r = nullptr; HANDLE leg = nullptr;
                    if (SUCCEEDED(d11->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
                    if (leg) {
                        write_katanga_handle(leg);
                        sharedTextureMutex  = mx;
                        sharedTexture_D3D11 = d11;
                        sharedTexture       = d11;
                        // WGL requires per-frame Lock/copy — do NOT set g_gl_ext_active
                        LOG_INF("SuperVrExport: OpenGL via WGL_NV_DX_interop2");
                        return;
                    }
                    if (mx) mx->Release();
                }
                d11->Release();
            }
            s_wglDXCloseDeviceNV(g_wgl_device); g_wgl_device = nullptr;
        }
    }

    // ── Path 3: CPU readback with PBO ping-pong (universal fallback) ─────────
    LOG_INF("SuperVrExport: OpenGL CPU PBO fallback");
    ID3D11Texture2D* d11 = create_d3d11_shared(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr);
    if (d11) sharedTexture = d11;
}

static void opengl_pbo_copy(GLuint gl_tex, reshade::api::device* dev,
                              reshade::api::resource src_res)
{
    if (!sharedTexture || !s_glGenBuffers) return;
    auto desc = dev->get_resource_desc(src_res);
    uint32_t w = desc.texture.width, h = desc.texture.height;
    DXGI_FORMAT fmt = reshade_to_dxgi(desc.texture.format);
    GLFormat glf = dxgi_to_gl(fmt);
    // Bytes per pixel based on DXGI format
    size_t px;
    switch (fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:  px = 8;  break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:  px = 16; break;
    default:                               px = 4;  break; // RGBA8, BGRA8, R10G10B10A2, R11G11B10
    }
    size_t sz = (size_t)w * h * px;

    if (g_pbo[0] == 0 || g_pbo_w != w || g_pbo_h != h) {
        if (g_pbo[0]) s_glDeleteBuffers(2, g_pbo);
        s_glGenBuffers(2, g_pbo);
        for (int i = 0; i < 2; ++i) {
            s_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[i]);
            s_glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)sz, nullptr, GL_STREAM_READ);
        }
        s_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        g_pbo_w = w; g_pbo_h = h; g_pbo_frame = 0;
    }

    uint32_t wi = g_pbo_frame % 2, ri = (g_pbo_frame + 1) % 2;
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    s_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[wi]);
    // FIX: use correct GL format/type for the texture
    glGetTexImage(GL_TEXTURE_2D, 0, glf.format, glf.type, nullptr);

    if (g_pbo_frame > 0) {
        s_glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[ri]);
        void* ptr = s_glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (ptr) {
            D3D11_BOX box = {0,0,0,w,h,1};
            bool pbo_mutex_ok = true;
            if (sharedTextureMutex) pbo_mutex_ok = SUCCEEDED(sharedTextureMutex->AcquireSync(0, 50)); // 50ms: matches original
            if (pbo_mutex_ok) {
                g_d3d11_context->UpdateSubresource(
                    static_cast<ID3D11Texture2D*>(sharedTexture), 0, &box,
                    ptr, (UINT)(w * px), 0);
                if (sharedTextureMutex) sharedTextureMutex->ReleaseSync(0);
            }
            s_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    s_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    ++g_pbo_frame;
}

// ── Texture lookup (cached) ───────────────────────────────────────────────────
reshade::api::effect_texture_variable get_vr_texture(reshade::api::effect_runtime* rt)
{
    if (g_cached_vr_tex.handle != 0 && !g_tex_cache_dirty) return g_cached_vr_tex;
    // Use nullptr effect name to search ALL effects regardless of how ReShade stores the path.
    auto v = rt->find_texture_variable(nullptr, "V__texTOT");
    if (v.handle == 0) v = rt->find_texture_variable(nullptr, "V__SuperDepth3D__DoubleTex");
    if (v.handle == 0) v = rt->find_texture_variable(nullptr, "V__SuperDepth3DVR__DoubleTex");
    if (v.handle == 0) v = rt->find_texture_variable(nullptr, "V__DoubleTex");
    if (v.handle != 0) { g_cached_vr_tex = v; g_tex_cache_dirty = false; }
    // If search failed but we have a stale cached handle, return it.
    // Better to copy from the old texture than to log "not found" every reload.
    if (v.handle == 0 && g_cached_vr_tex.handle != 0) return g_cached_vr_tex;
    return v;
}

reshade::api::resource get_texture_resource(reshade::api::effect_runtime* rt,
                                              reshade::api::effect_texture_variable t)
{
    reshade::api::resource_view a = {}, b = {};
    rt->get_texture_binding(t, &a, &b);
    if (a.handle || b.handle)
        return rt->get_device()->get_resource_from_view(b.handle ? b : a);
    return {0};
}

// ── Export / copy ──────────────────────────────────────────────────────────────
void export_effects(reshade::api::effect_runtime* rt)
{
    auto vt = get_vr_texture(rt);
    if (vt.handle == 0) return;
    auto res = get_texture_resource(rt, vt);
    if (!res.handle) return;
    // Skip re-setup if already sharing (avoids breaking KatanaVR handle on every reload).
    // Re-setup only happens when sharedTexture is null (first call or after resolution change).
    // Detect source dimension changes (e.g. game resolution change) and force re-setup.
    auto src_desc = rt->get_device()->get_resource_desc(res);
    uint32_t src_w = src_desc.texture.width, src_h = src_desc.texture.height;
    auto     src_f = src_desc.texture.format;
    cs_enter();
    if (sharedTexture && src_w == g_src_width && src_h == g_src_height && src_f == g_src_format) { cs_leave(); return; }
    // Source changed or first call — release and re-setup.
    if (sharedTexture) release_shared(rt->get_device()->get_api());
    g_src_width = src_w; g_src_height = src_h; g_src_format = src_f;
    auto* dev = rt->get_device();
    // get_native() returns uint64_t in API 14, void* in API 16+. Use uintptr_t for both.
    auto nat_raw = dev->get_native();
    void* nat = reinterpret_cast<void*>(static_cast<uintptr_t>(nat_raw));

    switch (dev->get_api()) {
    case reshade::api::device_api::d3d9:
        share_d3d9(reinterpret_cast<IDirect3DTexture9*>(res.handle),
                   reinterpret_cast<IDirect3DDevice9*>(nat)); break;
    case reshade::api::device_api::d3d10:
        share_d3d10(reinterpret_cast<ID3D10Texture2D*>(res.handle),
                    reinterpret_cast<ID3D10Device*>(nat)); break;
    case reshade::api::device_api::d3d11:
        share_d3d11(reinterpret_cast<ID3D11Texture2D*>(res.handle),
                    reinterpret_cast<ID3D11Device*>(nat),
                    reshade::api::device_api::d3d11); break;
    case reshade::api::device_api::d3d12:
        share_d3d12(reinterpret_cast<ID3D12Resource*>(res.handle),
                    reinterpret_cast<ID3D12Device*>(nat)); break;
    case reshade::api::device_api::vulkan:
        share_vulkan(res, dev); break;
    case reshade::api::device_api::opengl:
        share_opengl(static_cast<GLuint>(res.handle), res, dev); break;
    default:
        LOG_INF("SuperVrExport: Unsupported API"); break;
    }
    cs_leave();
}

void add_copy_command(reshade::api::effect_runtime* rt,
                      reshade::api::command_list*   cl,
                      reshade::api::resource_view   rtv,
                      reshade::api::resource_view   rtv_srgb)
{
    if (rt != g_active_runtime) return; // skip secondary runtimes
    auto vt = get_vr_texture(rt);
    if (vt.handle == 0) return;
    // Call get_texture_resource every frame — matches original addon.
    // Caching was removed: a stale pointer from a recompile invalidating texTOT
    // causes copy_resource to crash. The 2 virtual calls per frame are negligible.
    auto src = get_texture_resource(rt, vt);
    if (!src.handle || !sharedTexture) return;

    auto api = g_cached_api; // cached at init — avoids 2 virtual calls per frame

    // OpenGL: all three paths handled here — none fall through to copy_resource.
    if (api == reshade::api::device_api::opengl) {
        if (g_gl_ext_active) {
            return; // EXT: GPU-direct shared memory, nothing to do per-frame.
        }
        if (g_wgl_device && g_wgl_object && s_wglDXUnlockObjectsNV) {
            // WGL: GPU-direct shared memory. Lock was called in on_begin_effects_wgl.
            // Unlock here transfers ownership back to D3D11 for KatanaVR to read.
            s_wglDXUnlockObjectsNV(g_wgl_device, 1, &g_wgl_object);
            return;
        }
        opengl_pbo_copy(static_cast<GLuint>(src.handle), rt->get_device(), src);
        return;
    }

    reshade::api::resource dst;
    dst.handle = (api == reshade::api::device_api::vulkan && g_vk_image)
        ? static_cast<uint64_t>(g_vk_image)
        : uint64_t(sharedTexture);

    // No CS here — matches original artumino addon. Both export_effects and
    // add_copy_command fire on the render thread, so no cross-thread race on
    // sharedTexture. The CS in export_effects protects setup; the per-frame
    // copy path needs no lock.
    if (sharedTextureMutex)
        sharedTextureMutex->AcquireSync(0, 0); // 0ms: non-blocking

    rt->get_command_queue()->get_immediate_command_list()->copy_resource(src, dst);
    if (!sharedTextureMutex)
        rt->get_command_queue()->flush_immediate_command_list();

    if (sharedTextureMutex) sharedTextureMutex->ReleaseSync(0);


}

// ── Event handlers ─────────────────────────────────────────────────────────────
static void on_init_runtime(reshade::api::effect_runtime* rt)
{
    g_active_runtime   = rt;
    // Reset reload state on fresh init, but not after controller inserts
    // (g_vr_ready means we already found everything — don't reload again).
    if (!g_vr_ready) { g_reload_attempted = false; g_reload_count = 0; }
    g_tex_cache_dirty  = true;  // invalidate cache on new runtime (race-safe)
    g_cached_api       = rt->get_device()->get_api(); // cache API — never changes per runtime
    // Preprocessors are set in on_reloaded_with_fa only when texTOT is not found
    // (i.e. SuperDepth3D pipeline needed). Setting them here causes an immediate
    // SuperDepth3D recompile that destabilises 3DToElse texTOT on Geo3D games.
    // Moved to: on_reloaded_with_fa DoubleTex-not-found path.
}

static void on_reloaded_with_fa(reshade::api::effect_runtime* rt)
{
    g_fs_fa_handle      = find_sd3d_uniform(rt, "FS_FA");
    g_stereo_mode_handle = find_sd3d_uniform(rt, "Stereoscopic_Mode");
    g_tex_cache_dirty  = true;
    // Clear sharedTexture on reload.
    cs_enter();
    if (sharedTexture) {
        if (g_cached_api == reshade::api::device_api::opengl) {
            // OpenGL interop objects MUST be freed on every reload — they are
            // tied to the specific GL texture handle which changes after recompile.
            if (g_gl_mem_obj && s_glDeleteMemoryObjectsEXT) {
                s_glDeleteMemoryObjectsEXT(1, &g_gl_mem_obj); g_gl_mem_obj = 0;
            }
            if (g_wgl_object && g_wgl_device) {
                if (s_wglDXUnlockObjectsNV)    s_wglDXUnlockObjectsNV   (g_wgl_device, 1, &g_wgl_object);
                if (s_wglDXUnregisterObjectNV) s_wglDXUnregisterObjectNV(g_wgl_device, g_wgl_object);
                g_wgl_object = nullptr;
            }
            if (g_wgl_device && s_wglDXCloseDeviceNV) {
                s_wglDXCloseDeviceNV(g_wgl_device); g_wgl_device = nullptr;
            }
            g_gl_ext_active = false;
            release_shared(g_cached_api);
            g_src_width = 0; g_src_height = 0; g_src_format = SRC_FORMAT_UNSET;
        }
        // For D3D/Vulkan: do NOT release sharedTexture here.
        // export_effects will reuse it if source dims still match, keeping
        // KatangaMappedFile handle stable so VRScreenCap stays connected.
        // If dims changed, export_effects calls release_shared internally.
    }
    cs_leave();

    if (!g_reload_attempted) {
        auto vt_check = rt->find_texture_variable(nullptr, "V__texTOT");
        if (vt_check.handle == 0) vt_check = rt->find_texture_variable(nullptr, "V__SuperDepth3D__DoubleTex");
        if (vt_check.handle == 0) vt_check = rt->find_texture_variable(nullptr, "V__SuperDepth3DVR__DoubleTex");
        if (vt_check.handle == 0) vt_check = rt->find_texture_variable(nullptr, "V__DoubleTex");
        if (vt_check.handle == 0) {
            if (g_reload_count < 2) {
                LOG_ERR("SuperVrExport: DoubleTex not found — forcing reload");
                // Set SD3D preprocessors now — texTOT not present means SD3D pipeline needed.
                // Doing this here (not on_init) avoids breaking Geo3D games that already have texTOT.
                rt->set_preprocessor_definition("EX_DLP_FS_Mode",   "1");
                rt->set_preprocessor_definition("DoubleBuffer_Mode", "1");
                g_reload_attempted = true; ++g_reload_count; rt->reload_effect_next_frame(nullptr); return;
            }
            LOG_ERR("SuperVrExport: DoubleTex still missing — giving up");
        } else {
        }
    }

    // Only set mode on first stable setup — re-applying on every reload
    // causes a momentary Stereoscopic_Mode reset that glitches frame sequential output.
    apply_fa_state(rt, true);
    export_effects(rt);
    // g_cached_vr_tex already populated by export_effects if found — no re-search needed.
    if (g_cached_vr_tex.handle != 0) {
        g_vr_ready = true;
    }
}

// Forward declarations for functions registered in DllMain.
static void on_destroy_runtime(reshade::api::effect_runtime*);
static void on_begin_effects_wgl(reshade::api::effect_runtime*, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view);
static void on_finish_effects_with_fa(reshade::api::effect_runtime*, reshade::api::command_list*, reshade::api::resource_view, reshade::api::resource_view);

// ── DllMain ─────────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        InitializeCriticalSection(&g_cs); g_cs_init = true;
        if (!reshade::register_addon(hModule)) return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_runtime);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_with_fa);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects_wgl);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects_with_fa);
        break;
    case DLL_PROCESS_DETACH:
        g_reload_count = 0;
        g_vr_ready = false;
        if (g_active_runtime) apply_fa_state(g_active_runtime, false);
        if (g_d3d9_shared_tex) { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = nullptr; }
        if (g_vk_image  && s_vkDestroyImage) s_vkDestroyImage(g_vk_device, g_vk_image,  nullptr);
        if (g_vk_memory && s_vkFreeMemory)   s_vkFreeMemory  (g_vk_device, g_vk_memory, nullptr);
        if (g_pbo[0] && s_glDeleteBuffers)   s_glDeleteBuffers(2, g_pbo);
        if (g_gl_mem_obj && s_glDeleteMemoryObjectsEXT) s_glDeleteMemoryObjectsEXT(1, &g_gl_mem_obj);
        if (g_wgl_object && g_wgl_device && s_wglDXUnregisterObjectNV) s_wglDXUnregisterObjectNV(g_wgl_device, g_wgl_object);
        if (g_wgl_device && s_wglDXCloseDeviceNV) s_wglDXCloseDeviceNV(g_wgl_device);
        if (sharedTextureMutex) sharedTextureMutex->Release();
        if (sharedTexture_D3D11) static_cast<ID3D11Texture2D*>(sharedTexture_D3D11)->Release();
        if (sharedTexture && sharedTexture != sharedTexture_D3D11
            && sharedTexture != reinterpret_cast<void*>(static_cast<uintptr_t>(1u))) {
            static_cast<IUnknown*>(sharedTexture)->Release();
        }
        // Do NOT close KatangaMappedFile on DLL unload.
        // Keeping it alive means reload finds the existing mapping,
        // so KatanaVR stays connected. OS cleans up at process exit.
        if (g_d3d11_context) { g_d3d11_context->Release(); g_d3d11_context = nullptr; }
        if (g_d3d11_device)  { g_d3d11_device->Release();  g_d3d11_device  = nullptr; }
        reshade::unregister_addon(hModule);
        if (g_cs_init) { DeleteCriticalSection(&g_cs); g_cs_init = false; }
        break;
    }
    return TRUE;
}

static void on_begin_effects_wgl(reshade::api::effect_runtime* rt,
    reshade::api::command_list*,
    reshade::api::resource_view,
    reshade::api::resource_view)
{
    if (rt == g_active_runtime &&
        g_wgl_device && g_wgl_object && s_wglDXLockObjectsNV)
        s_wglDXLockObjectsNV(g_wgl_device, 1, &g_wgl_object);
}

static void on_destroy_runtime(reshade::api::effect_runtime* rt)
{
    apply_fa_state(rt, false);
    if (g_active_runtime == rt) {
        g_active_runtime = nullptr;
        cs_enter();
        release_shared(g_cached_api); // use cached — avoids 2 virtual calls
        if (g_d3d9_shared_tex) { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = nullptr; }
        g_src_width = 0; g_src_height = 0; g_src_format = SRC_FORMAT_UNSET;
        g_vk_width  = 0; g_vk_height  = 0;
        cs_leave();
    }
}

static void on_finish_effects_with_fa(reshade::api::effect_runtime* rt,
    reshade::api::command_list*  cl,
    reshade::api::resource_view  rtv,
    reshade::api::resource_view  rtv_srgb)
{
    add_copy_command(rt, cl, rtv, rtv_srgb);
}
