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
// D3D12 NATIVE path: named NT handle for the "DX12VRStream" shared resource.
// When non-null, the D3D12 bridge created the shared texture directly on the game's
// D3D12 device (no D3D11 bridge) and VRScreenCap opens it via OpenSharedHandleByName.
static HANDLE           g_d3d12_named_handle = nullptr;
static ID3D12Device*    g_d3d12_native_dev   = nullptr; // D3D12 device our current d3d12 sharedTexture belongs to (native OR bridge)

// Copy-completion fence (native D3D12 path only). VRScreenCap samples the shared
// texture live with NO keyed mutex / no acquire on its side (confirmed in
// katanga_loader.rs — it OpenSharedHandleByName + samples, never AcquireSync), so the
// only way to stop it reading a half-written copy_resource is to make the producer
// wait until the copy is GPU-complete before the next frame overwrites it. A real
// queue-ordered ID3D12Fence (Signal on the queue, bounded host wait) does this; it is
// scoped to the copy, not a full wait_idle() drain.
static ID3D12Fence*     g_d3d12_copy_fence   = nullptr;
static HANDLE           g_d3d12_fence_event  = nullptr;
static UINT64           g_d3d12_fence_value  = 0;
// FIX: single persistent file mapping handle (no leak on repeated calls)
static HANDLE           g_katanga_mapping   = nullptr;
static DWORD*           g_katanga_view      = nullptr;

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

// (D3D12 uses ReShade's immediate command list for copies — see add_copy_command)
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
static reshade::api::effect_uniform_variable g_fs_fa_handle        = { 0 };
static reshade::api::effect_uniform_variable g_stereo_mode_handle  = { 0 };

static void apply_fa_state(reshade::api::effect_runtime* rt, bool on)
{
    if (!on) {
        if (g_stereo_mode_handle.handle) { int32_t m=0; rt->set_uniform_value_int(g_stereo_mode_handle,&m,1); }
        if (g_fs_fa_handle.handle)       { bool f=false; rt->set_uniform_value_bool(g_fs_fa_handle,&f,1); }
        return;
    }
    // Stereoscopic_Mode = 0 (Side by Side). With DoubleBuffer_Mode on, SuperDepth3D
    // writes the full-res SBS image into DoubleTex (BUFFER_WIDTH*2 x BUFFER_HEIGHT)
    // EVERY frame — both eyes present in one texture, exactly what VRScreenCap's
    // hardcoded StereoMode::FullSbs expects.
    //
    // The DoubleTex SBS-write branch in SuperDepth3D (PS, ~line 7398) only runs when
    // VR_Stereoscopic_Mode() is 0 or 1. In Frame Sequential mode 6 that branch never
    // executes, so DoubleTex is left as mono/garbage — which is the broken double-wide
    // mono image seen in the overlay. Mode 0 is required for DoubleTex to be valid SBS.
    // FS_FA / Frame_Alternate are not used in this path.
    if (g_stereo_mode_handle.handle) { int32_t m=0; rt->set_uniform_value_int(g_stereo_mode_handle, &m, 1); }
    if (g_fs_fa_handle.handle)       { bool f=false; rt->set_uniform_value_bool(g_fs_fa_handle, &f, 1); }
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
    // Recreate mapping every call so the consumer detects the new handle.
    // The consumer opens KatangaMappedFile by name — closing and recreating
    // signals it to re-open and pick up the new D3D resource handle.
    //
    // Size = 8 bytes (pointer width). VRScreenCap reads the value as a `usize`
    // (8 bytes on x64); bo3b Katanga reads only the low 4 bytes as a UINT. Writing
    // the full 64-bit value with zeroed upper bytes satisfies both: Katanga gets the
    // low 32-bit KMT handle, VRScreenCap gets the exact pointer-width value.
    if (g_katanga_view)    { UnmapViewOfFile(g_katanga_view);  g_katanga_view    = nullptr; }
    if (g_katanga_mapping) { CloseHandle(g_katanga_mapping);   g_katanga_mapping = nullptr; }
    g_katanga_mapping = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(uint64_t), L"Local\\KatangaMappedFile");
    if (!g_katanga_mapping) return;
    g_katanga_view = reinterpret_cast<DWORD*>(MapViewOfFile(
        g_katanga_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(uint64_t)));
    if (g_katanga_view)
        *reinterpret_cast<uint64_t*>(g_katanga_view) = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
}

// Forward declaration — defined and initialised here (used in release_shared above share_d3d9).
static IDirect3DTexture9* g_d3d9_shared_tex = nullptr;

// FIX: unified cleanup — releases sharedTexture and sharedTexture_D3D11 safely
static void release_shared(reshade::api::device_api api)
{
    // sharedTextureMutex == (IDXGIKeyedMutex*)1 is a sentinel meaning "no mutex, skip flush"
    // for D3D11 plain SHARED path. Don't call Release() on it.
    if (sharedTextureMutex && sharedTextureMutex != reinterpret_cast<IDXGIKeyedMutex*>(1)) {
        sharedTextureMutex->Release();
    }
    sharedTextureMutex = nullptr;
    // Release the API-specific copy dst
    if (sharedTexture && sharedTexture != sharedTexture_D3D11) {
        switch (api) {
        case reshade::api::device_api::d3d11:
            static_cast<ID3D11Texture2D*>(sharedTexture)->Release(); break;
        case reshade::api::device_api::d3d12:
            static_cast<ID3D12Resource*>(sharedTexture)->Release();
            // Native D3D12 path: close the named "DX12VRStream" NT handle.
            if (g_d3d12_named_handle) { CloseHandle(g_d3d12_named_handle); g_d3d12_named_handle = nullptr; }
            g_d3d12_native_dev = nullptr;
            // Release the copy-completion fence + event (recreated on next native share).
            if (g_d3d12_copy_fence) { g_d3d12_copy_fence->Release(); g_d3d12_copy_fence = nullptr; }
            if (g_d3d12_fence_event) { CloseHandle(g_d3d12_fence_event); g_d3d12_fence_event = nullptr; }
            g_d3d12_fence_value = 0;
            break;
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
// allow_rtv: when false, omit the RENDER_TARGET bind flag. The shared texture is never
// rendered to — it is only a copy_resource destination and a shader resource KatanaVR
// samples. RENDER_TARGET forces a render-target layout which enables DCC (delta color
// compression) on modern GPUs; DCC must be decompressed on every cross-device/cross-engine
// access, which is precisely the per-frame cross-device copy the D3D12 bridge performs.
// Dropping RENDER_TARGET keeps the texture in a plain, directly-shareable layout.
static ID3D11Texture2D* create_d3d11_shared(
    uint32_t w, uint32_t h, DXGI_FORMAT fmt, HANDLE* out_nt,
    IDXGIAdapter* adapter = nullptr, bool allow_rtv = true)
{
    if (!ensure_d3d11_device(adapter)) return nullptr;
    fmt = dxgi_ensure_typed(fmt);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt; d.SampleDesc = {1,0}; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = allow_rtv
        ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)
        : D3D11_BIND_SHADER_RESOURCE; // copy-dst + KatanaVR SRV only; no RT layout/DCC
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
    if (sharedTextureMutex && sharedTextureMutex != reinterpret_cast<IDXGIKeyedMutex*>(1)) {
        sharedTextureMutex->Release();
    }
    sharedTextureMutex  = nullptr; // no keyed mutex with SHARED flag
    if (sharedTexture_D3D11) { static_cast<ID3D11Texture2D*>(sharedTexture_D3D11)->Release(); sharedTexture_D3D11 = nullptr; }
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
    d.MiscFlags    = D3D11_RESOURCE_MISC_SHARED; // KEYEDMUTEX breaks GetSharedHandle (returns null) — plain SHARED works
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
        sharedTextureMutex = reinterpret_cast<IDXGIKeyedMutex*>(1); // sentinel: D3D11 copies are synchronous
        LOG_INF("SuperVrExport: D3D11 ready (Unity standalone bridge)");
        return;
    }
    ID3D11Texture2D* shared = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &shared))) {
        LOG_ERR("SuperVrExport: D3D11 CreateTexture2D failed"); return;
    }
    // Plain SHARED: no IDXGIKeyedMutex available (incompatible with SHARED flag).
    // D3D11 immediate context copy_resource is synchronous — no mutex or flush needed.
    // Set sharedTextureMutex to a sentinel value (1) so add_copy_command skips
    // flush_immediate_command_list (which would stall the GPU every frame on D3D11).
    IDXGIResource* r = nullptr; HANDLE leg = nullptr;
    if (SUCCEEDED(shared->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
    if (!leg) { shared->Release(); LOG_ERR("SuperVrExport: D3D11 GetSharedHandle failed"); return; }

    write_katanga_handle(leg);
    sharedTextureMutex  = reinterpret_cast<IDXGIKeyedMutex*>(1); // sentinel: non-null = skip flush, no AcquireSync
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
    d.MiscFlags    = D3D10_RESOURCE_MISC_SHARED; // KEYEDMUTEX breaks GetSharedHandle — plain SHARED works

    ID3D10Texture2D* shared = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &shared))) {
        LOG_ERR("SuperVrExport: D3D10 CreateTexture2D failed"); return;
    }
    IDXGIResource* r = nullptr; HANDLE leg = nullptr;
    if (SUCCEEDED(shared->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
    if (!leg) { shared->Release(); LOG_ERR("SuperVrExport: D3D10 GetSharedHandle failed"); return; }

    write_katanga_handle(leg);
    sharedTextureMutex  = reinterpret_cast<IDXGIKeyedMutex*>(1); // sentinel: skip flush, D3D10 copies are synchronous
    sharedTexture_D3D11 = nullptr;
    sharedTexture       = shared;
    LOG_INF("SuperVrExport: D3D10 ready");
}

static void share_d3d9(IDirect3DTexture9* src, IDirect3DDevice9* dev)
{
    // release_shared handles both the surface (sharedTexture) and g_d3d9_shared_tex.
    // Do NOT release g_d3d9_shared_tex manually here — the surface (sharedTexture) refs
    // the same memory and release_shared releases the surface first, then the texture.
    // Releasing the texture first causes use-after-free when release_shared then
    // releases the surface whose owning texture is already gone.
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


// D3D12 NATIVE path (VRScreenCap fast path): create the shared texture directly on the
// GAME's D3D12 device, named "DX12VRStream", and let VRScreenCap open it by name via
// ID3D12Device::OpenSharedHandleByName (it imports D3D12 resources as Vulkan external
// memory with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE). This eliminates the
// standalone D3D11 device and the cross-DEVICE copy: the per-frame copy_resource becomes
// same-device D3D12 -> D3D12. ALLOW_SIMULTANEOUS_ACCESS keeps the resource in COMMON state
// so a separate process/device can read it concurrently without a keyed mutex — the proper
// D3D12 mechanism for exactly this producer/consumer pattern, which also avoids the
// NVIDIA 580.88+ D3D12<->D3D11 cross-process regression entirely (no D3D11 surface involved).
//
// VRScreenCap tries D3D11 OpenSharedResource FIRST and only falls back to the D3D12
// by-name path when that fails. We write the (process-local) NT handle value to
// KatangaMappedFile: it is not a valid D3D11 KMT handle in VRScreenCap's process, so its
// D3D11 attempt fails and it falls through to OpenSharedHandleByName("DX12VRStream").
//
// Returns true on success. On any failure the caller falls back to the D3D11 bridge,
// which keeps compatibility with games/drivers that disallow D3D12 shared heaps and with
// the bo3b Katanga (D3D11-only) consumer.
static bool share_d3d12_native(ID3D12Resource* src, ID3D12Device* dev)
{
    DXGI_FORMAT fmt = dxgi_ensure_typed(src->GetDesc().Format);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Build a clean minimal desc — do NOT copy src->GetDesc().Flags wholesale.
    // texTOT may carry flags (e.g. ALLOW_UNORDERED_ACCESS, ALLOW_DEPTH_STENCIL, or
    // engine-specific private flags) that are incompatible with D3D12_HEAP_FLAG_SHARED.
    // Blindly OR-ing those causes CreateCommittedResource to fail silently (S_OK but
    // null resource, or E_INVALIDARG) and fall back to the D3D11 bridge.
    // Instead, start from the source dims/format/layout, then set only the two flags
    // we actually need: SIMULTANEOUS_ACCESS (shared cross-process read, no mutex) and
    // ALLOW_RENDER_TARGET (VRScreenCap may sample it as an SRV, needs RT layout compat).
    D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment        = 0;
    desc.Width            = srcDesc.Width;
    desc.Height           = srcDesc.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = fmt; // already resolved to typed by dxgi_ensure_typed above
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
                          | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* res = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
    if (FAILED(hr) || !res) {
        // SIMULTANEOUS_ACCESS + SHARED is rejected by some drivers. Retry without the
        // simultaneous-access flag (keep render-target): a plain COMMON shared resource
        // still works as a copy destination via implicit COMMON->COPY_DEST promotion,
        // and the cross-process consumer reads it via Vulkan external memory.
        char m1[128]; snprintf(m1, sizeof(m1),
            "SuperVrExport: D3D12 native CreateCommittedResource(SIMULTANEOUS) hr=0x%08X — retrying plain", (unsigned)hr);
        LOG_INF(m1);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
    }
    if (FAILED(hr) || !res) {
        char m2[128]; snprintf(m2, sizeof(m2),
            "SuperVrExport: D3D12 native shared resource unavailable (hr=0x%08X) — using D3D11 bridge", (unsigned)hr);
        LOG_INF(m2);
        return false;
    }

    HANDLE nt = nullptr;
    hr = dev->CreateSharedHandle(res, nullptr, GENERIC_ALL, L"DX12VRStream", &nt);
    if (FAILED(hr) || !nt) {
        char m3[128]; snprintf(m3, sizeof(m3),
            "SuperVrExport: D3D12 CreateSharedHandle(DX12VRStream) hr=0x%08X — using D3D11 bridge", (unsigned)hr);
        LOG_INF(m3);
        res->Release();
        return false;
    }


    // Publish to KatangaMappedFile. The value makes VRScreenCap's D3D11 attempt fail so it
    // falls back to OpenSharedHandleByName("DX12VRStream"). The name carries the resource.
    write_katanga_handle(nt);

    g_d3d12_named_handle = nt;   // keep open: the name stays registered while the handle lives
    g_d3d12_native_dev   = dev;  // remember owning device to detect device recreation
    sharedTexture        = res;  // copy_resource dst (same D3D12 device as texTOT)
    sharedTexture_D3D11  = nullptr;
    sharedTextureMutex   = nullptr; // no flush; SIMULTANEOUS_ACCESS handles concurrent read

    // Create the copy-completion fence + event (best-effort; copy still works without it,
    // just without the tear-window guarantee). Reuse across recreates if already present.
    if (!g_d3d12_copy_fence) {
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&g_d3d12_copy_fence)))) {
            g_d3d12_copy_fence = nullptr; // proceed unsynchronized rather than fail the share
        } else {
            g_d3d12_fence_value = 0;
            if (!g_d3d12_fence_event)
                g_d3d12_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
    }

    char rdy[160]; snprintf(rdy, sizeof(rdy),
        "SuperVrExport: D3D12 ready (NATIVE same-device, fmt=0x%X, named DX12VRStream to VRScreenCap)",
        (unsigned)fmt);
    LOG_INF(rdy);
    return true;
}

static void share_d3d12(ID3D12Resource* src, ID3D12Device* dev)
{
    release_shared(reshade::api::device_api::d3d12);

    // Fast path: native same-device D3D12 shared texture (VRScreenCap DX12VRStream).
    if (share_d3d12_native(src, dev)) return;

    // Fallback: reversed D3D11 bridge — create D3D11 shared tex -> open as D3D12 copy dst
    // on the GAME device. Used when the native D3D12 shared resource can't be created
    // (driver/game restriction) or for the D3D11-only bo3b Katanga consumer.
    // VRScreenCap/Katanga read via the D3D11 KMT handle written to KatangaMappedFile.
    if (!ensure_d3d11_device()) { LOG_ERR("SuperVrExport: D3D12 bridge: no D3D11 device"); return; }

    D3D12_RESOURCE_DESC rd = src->GetDesc();
    DXGI_FORMAT src_fmt = dxgi_ensure_typed(rd.Format);

    // Use game D3D12 device's adapter for our standalone D3D11 device.
    // Cross-adapter shared handles (e.g. integrated vs discrete GPU) produce black frames.
    IDXGIDevice* dxgi_dev12 = nullptr;
    IDXGIAdapter* game_adapter = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev12)))) {
        dxgi_dev12->GetAdapter(&game_adapter); dxgi_dev12->Release();
    }
    HANDLE nt_for_d3d12 = nullptr;
    ID3D11Texture2D* d11 = create_d3d11_shared((UINT)rd.Width, (UINT)rd.Height,
        src_fmt, &nt_for_d3d12, game_adapter, /*allow_rtv=*/false);
    if (game_adapter) { game_adapter->Release(); game_adapter = nullptr; }
    if (!d11) { LOG_ERR("SuperVrExport: D3D12 bridge: create_d3d11_shared failed"); return; }
    if (!nt_for_d3d12) { LOG_ERR("SuperVrExport: D3D12 bridge: KMT handle from D3D11 failed"); return; }
    // Open the D3D11 shared texture on the game D3D12 device using the KMT handle.
    // D3D12::OpenSharedHandle accepts KMT handles from D3D11 SHARED textures.
    // DO NOT CloseHandle on KMT handles — they are not reference-counted like NT handles.
    ID3D12Resource* d12 = nullptr;
    if (FAILED(dev->OpenSharedHandle(nt_for_d3d12, IID_PPV_ARGS(&d12)))) {
        LOG_ERR("SuperVrExport: D3D12 bridge: OpenSharedHandle on game D3D12 device failed");
        return;
    }

    // KatangaMappedFile already written by create_d3d11_shared above.
    sharedTexture = d12;
    g_d3d12_native_dev = dev; // track owning device so a device recreate forces rebuild

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
            // Fall through to next path — must clear sharedTexture_D3D11 before releasing d11.
            // create_d3d11_shared set sharedTexture_D3D11 = d11; releasing d11 without
            // clearing it leaves sharedTexture_D3D11 dangling → crash on next release_shared.
        }
        if (d11) {
            if (sharedTexture_D3D11 == static_cast<void*>(d11)) sharedTexture_D3D11 = nullptr;
            d11->Release();
        }
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
    // Prefer DoubleTex (SuperDepth3D's direct full SBS output, BUFFER_WIDTH*2 wide) so
    // 3DToElse can be skipped entirely — DoubleTex already holds both eyes per frame at
    // full per-eye resolution, which is exactly what VRScreenCap's FullSbs expects and is
    // higher quality than 3DToElse's screen-res texTOT. texTOT is the fallback for setups
    // that still run 3DToElse (or Geo3D, where DoubleTex doesn't exist).
    auto v = rt->find_texture_variable(nullptr, "V__SuperDepth3D__DoubleTex");
    if (v.handle == 0) v = rt->find_texture_variable(nullptr, "V__SuperDepth3DVR__DoubleTex");
    if (v.handle == 0) v = rt->find_texture_variable(nullptr, "V__DoubleTex");
    if (v.handle == 0) v = rt->find_texture_variable(nullptr, "V__texTOT");
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
    // Fast non-CS dims check: if already sharing at same resolution, skip setup.
    // This avoids taking the CS on every reload (which causes deadlock when
    // D3D11CreateDevice fires ReShade hooks that try to re-enter export_effects).
    // If a D3D12 shared resource (native OR bridge) was retained across a runtime
    // recreate but the D3D12 device itself changed, the retained resource is dead —
    // force a rebuild so we don't copy into a resource owned by a destroyed device.
    if (sharedTexture && g_d3d12_native_dev &&
        rt->get_device()->get_api() == reshade::api::device_api::d3d12 &&
        reinterpret_cast<ID3D12Device*>(static_cast<uintptr_t>(rt->get_device()->get_native())) != g_d3d12_native_dev) {
        cs_enter();
        release_shared(reshade::api::device_api::d3d12);
        g_src_width = 0; g_src_height = 0; g_src_format = SRC_FORMAT_UNSET;
        g_d3d12_native_dev = nullptr;
        cs_leave();
    }
    if (sharedTexture && src_w == g_src_width && src_h == g_src_height && src_f == g_src_format) return;
    cs_enter();
    // Re-check under CS in case another thread just set up the bridge.
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

    // ── Copy path for D3D9/D3D10/D3D11/D3D12/Vulkan ───────────────────────────
    // copy_resource is recorded on ReShade's immediate command list. Sync regime is
    // selected by the value of sharedTextureMutex:
    //
    //   real mutex  (WGL OpenGL): AcquireSync/ReleaseSync provide cross-device sync.
    //   sentinel(1) (D3D11/D3D10): plain SHARED, same-device synchronous copy on the
    //               immediate context. No mutex methods callable.
    //   nullptr     (D3D12/Vulkan): plain SHARED, no keyed mutex (Katanga's protocol
    //               needs a 32-bit KMT handle; KEYEDMUTEX would need a 64-bit NT handle).
    //
    // Per-frame flush policy:
    //   - D3D11/D3D10/D3D9/Vulkan/OpenGL and the D3D12 *bridge* path: NO flush. On the
    //     cross-device bridge a flush was catastrophic (it drained a cross-device copy
    //     mid-frame -> latency + jitter). ReShade flushes at Present anyway.
    //   - D3D12 *native same-device* path (g_d3d12_named_handle != null): flush. This
    //     matches the original addon, which always flushed. On the native path the copy
    //     is same-device (texTOT -> our D3D12 resource on the SAME device), so the flush
    //     just submits a cheap local copy so it completes before VRScreenCap's next
    //     cross-process read — shrinking the no-mutex tear window that causes flicker.
    //     This is a fundamentally cheaper flush than the bridge one we removed earlier.
    static const auto kSentinel = reinterpret_cast<IDXGIKeyedMutex*>(1);
    const bool realMutex = (sharedTextureMutex && sharedTextureMutex != kSentinel);

    if (realMutex) sharedTextureMutex->AcquireSync(0, 0); // 0ms: non-blocking

    rt->get_command_queue()->get_immediate_command_list()->copy_resource(src, dst);

    // Native D3D12 only: submit the same-device copy now so VRScreenCap reads a complete
    // frame. g_d3d12_named_handle is non-null only when share_d3d12_native succeeded.
    if (g_d3d12_named_handle) {
        rt->get_command_queue()->flush_immediate_command_list();

        // Close the tear window: wait until the copy is GPU-complete before returning,
        // so VRScreenCap (which samples the shared texture live, with no acquire on its
        // side) never catches a half-written copy. This is a queue-ordered ID3D12Fence
        // Signal + bounded host wait — scoped to the copy, NOT a full wait_idle() drain,
        // so it doesn't stall the game's whole queue. Bounded timeout prevents any hang
        // if the GPU is saturated; on timeout we just proceed (a rare single-frame tear
        // is preferable to a stall). All best-effort: skipped if the fence is absent.
        if (g_d3d12_copy_fence && g_d3d12_fence_event) {
            auto* q = reinterpret_cast<ID3D12CommandQueue*>(
                static_cast<uintptr_t>(rt->get_command_queue()->get_native()));
            if (q) {
                const UINT64 target = ++g_d3d12_fence_value;
                if (SUCCEEDED(q->Signal(g_d3d12_copy_fence, target))) {
                    if (g_d3d12_copy_fence->GetCompletedValue() < target) {
                        if (SUCCEEDED(g_d3d12_copy_fence->SetEventOnCompletion(
                                target, g_d3d12_fence_event))) {
                            // 8 ms cap: ~half a 60fps frame. Long enough for a local
                            // SBS copy to finish, short enough to never visibly hitch.
                            WaitForSingleObject(g_d3d12_fence_event, 8);
                        }
                    }
                }
            }
        }
    }

    if (realMutex) sharedTextureMutex->ReleaseSync(0);
}

// ── Event handlers ─────────────────────────────────────────────────────────────
static void on_init_runtime(reshade::api::effect_runtime* rt)
{
    // Only track the first runtime we see as the active one.
    // RE9 and other RE Engine games create a secondary 1x1 composition swapchain
    // after the main one. If we overwrite g_active_runtime with it, add_copy_command
    // returns immediately on every frame and the VR export stops.
    // on_destroy_runtime clears g_active_runtime, so the next init after a real
    // destroy correctly re-sets it to the new main runtime.
    if (!g_active_runtime) {
        g_active_runtime = rt;
        g_cached_api     = rt->get_device()->get_api();
    }
    if (rt == g_active_runtime) {
        g_tex_cache_dirty = true;  // invalidate cache on new runtime (race-safe)
        g_cached_api      = rt->get_device()->get_api(); // refresh on recreate
        // Set SD3D preprocessors on every init so they're in place before the first
        // compile — even if the user hasn't set them manually. This is safe to call
        // repeatedly; ReShade only recompiles if the value actually changed.
        if (!g_vr_ready) {
            rt->set_preprocessor_definition("EX_DLP_FS_Mode",   "1");
            rt->set_preprocessor_definition("DoubleBuffer_Mode", "1");
        }
    }
}

static void on_reloaded_with_fa(reshade::api::effect_runtime* rt)
{
    // Skip secondary runtimes — FA handles and shared state belong to g_active_runtime.
    // If a secondary swapchain reloads, searching it would zero out g_fs_fa_handle
    // (not found there) and disable frame sequential on the main swapchain.
    if (rt != g_active_runtime) return;
    g_fs_fa_handle        = find_sd3d_uniform(rt, "FS_FA");
    g_stereo_mode_handle  = find_sd3d_uniform(rt, "Stereoscopic_Mode");
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

    // No reload cascade. texTOT/DoubleTex is always present once SuperDepth3D
    // compiles — the preprocessors EX_DLP_FS_Mode and DoubleBuffer_Mode are set
    // by the user and don't need the addon to set them. If texTOT isn't found yet
    // (runtime just recreated, shaders not assigned yet), just return and wait for
    // the next reshade_reloaded_effects which fires once shaders are ready.

    // export_effects populates g_cached_vr_tex — must run first.
    export_effects(rt);
    // Only apply FA state once DoubleTex is confirmed present.
    // During the reload cascade (DoubleTex not yet found) the runtime is
    // destroyed/recreated immediately after, resetting any uniforms we set here.
    // Applying mid-cascade causes the FS mode to flicker on/off.
    //
    // Mid-session reloads (g_vr_ready already true): RE9 scene transitions fire
    // reshade_reloaded_effects multiple times in rapid succession. Re-applying
    // apply_fa_state on every one spams Stereoscopic_Mode writes while the shader
    // is still mid-recompile, causing the same flicker. Only re-apply if we were
    // previously not ready (first successful setup) OR if the uniform handles were
    // refreshed (g_fs_fa_handle may change after a recompile invalidates old handles).
    if (g_cached_vr_tex.handle != 0) {
        const bool was_ready = g_vr_ready;
        g_vr_ready        = true;
        // Apply FA state on first successful setup, or after a runtime destroy/recreate
        // that clears g_vr_ready. Do NOT re-apply on every mid-session recompile —
        // that spams mode writes while the shader is unstable.
        if (!was_ready) {
            apply_fa_state(rt, true);
        }
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
        g_vr_ready = false;
        if (g_active_runtime) apply_fa_state(g_active_runtime, false);
        if (g_d3d9_shared_tex) { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = nullptr; }
        if (g_vk_image  && s_vkDestroyImage) s_vkDestroyImage(g_vk_device, g_vk_image,  nullptr);
        if (g_vk_memory && s_vkFreeMemory)   s_vkFreeMemory  (g_vk_device, g_vk_memory, nullptr);
        if (g_pbo[0] && s_glDeleteBuffers)   s_glDeleteBuffers(2, g_pbo);
        if (g_gl_mem_obj && s_glDeleteMemoryObjectsEXT) s_glDeleteMemoryObjectsEXT(1, &g_gl_mem_obj);
        if (g_wgl_object && g_wgl_device && s_wglDXUnregisterObjectNV) s_wglDXUnregisterObjectNV(g_wgl_device, g_wgl_object);
        if (g_wgl_device && s_wglDXCloseDeviceNV) s_wglDXCloseDeviceNV(g_wgl_device);
        if (sharedTextureMutex && sharedTextureMutex != reinterpret_cast<IDXGIKeyedMutex*>(1))
            sharedTextureMutex->Release();
        if (sharedTexture_D3D11) static_cast<ID3D11Texture2D*>(sharedTexture_D3D11)->Release();
        if (sharedTexture && sharedTexture != sharedTexture_D3D11
            && sharedTexture != reinterpret_cast<void*>(static_cast<uintptr_t>(1u))) {
            static_cast<IUnknown*>(sharedTexture)->Release();
        }
        if (g_d3d12_named_handle) { CloseHandle(g_d3d12_named_handle); g_d3d12_named_handle = nullptr; }
        if (g_d3d12_copy_fence)  { g_d3d12_copy_fence->Release(); g_d3d12_copy_fence = nullptr; }
        if (g_d3d12_fence_event) { CloseHandle(g_d3d12_fence_event); g_d3d12_fence_event = nullptr; }
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
    // Only revert FA state if fully running — not during the initial reload cascade
    // where g_vr_ready is still false. Calling apply_fa_state(false) mid-cascade
    // resets Stereoscopic_Mode/FS_FA on the runtime on_reloaded_with_fa just
    // configured, contributing to frame-sequential flicker.
    if (g_vr_ready && rt == g_active_runtime) apply_fa_state(rt, false);
    if (g_active_runtime == rt) {
        // Reset g_vr_ready so the next on_reloaded_with_fa re-applies FA state.
        // Without this, g_vr_ready persists true across D3D12 scene transitions:
        // the bridge is torn down but the uniform state (Stereoscopic_Mode, FS_FA)
        // is lost when the runtime is destroyed — it must be re-written on next reload.
        g_vr_ready = false;
        g_active_runtime = nullptr;
        cs_enter();
        auto api = g_cached_api;
        // D3D11 and D3D12-native: do NOT release the shared resource on destroy.
        // The runtime is frequently destroyed/recreated at the SAME resolution by
        // events that have nothing to do with the swapchain — enabling a virtual
        // controller, scene transitions, alt-tab. Tearing down the native D3D12
        // shared resource here would close the named "DX12VRStream" handle, but
        // VRScreenCap still holds that name open, so the subsequent recreate's
        // CreateSharedHandle("DX12VRStream") FAILS (name still referenced) and the
        // addon falls back to the D3D11 bridge with a different KMT handle —
        // breaking Katanga's connection. Keep the native resource + named handle
        // alive across the recreate; export_effects' dimension check reuses it if
        // the resolution is unchanged, or calls release_shared itself if it changed.
        // Keep-alive policy across a runtime destroy/recreate (controller toggle,
        // alt-tab, scene transition — none of which change resolution):
        //
        // The texture KatangaVR/VRScreenCap actually reads is, for several APIs, owned
        // by OUR resources (the native D3D12 shared resource, or a texture on our
        // standalone D3D11 bridge device) — NOT by the game's runtime. Those survive a
        // runtime recreate intact, so releasing them here only churns the KatangaMappedFile
        // handle and drops the VR connection. Keep them; export_effects' dimension check
        // reuses them at the same resolution or rebuilds if it actually changed.
        //
        //   d3d11        : Katanga texture on game device, survives ResizeBuffers   -> keep
        //   d3d12 native : our shared resource + named "DX12VRStream" handle        -> keep
        //   d3d12 bridge : Katanga texture on our standalone D3D11 device           -> keep
        //   d3d9         : game-side surface tied to game D3D9 device (recreated)    -> release
        //   vulkan       : g_vk_image lives on the game's VK device (recreated)     -> release
        //   opengl       : interop objects tied to the GL context (recreated)       -> release
        //   d3d10        : Katanga texture on the game's D3D10 device               -> release
        const bool keepAlive =
            api == reshade::api::device_api::d3d11 ||
            (api == reshade::api::device_api::d3d12); // native OR bridge: Katanga side is ours
        // D3D9 is intentionally NOT kept alive: its game-side surface (g_d3d9_shared_tex)
        // is tied to the game's D3D9 device and is released below regardless, so the
        // Katanga chain must be rebuilt. D3D10/Vulkan/OpenGL rebuild for similar reasons
        // (game-device or context-bound objects).

        if (keepAlive) {
            // Intentionally keep sharedTexture / sharedTexture_D3D11 / g_d3d12_named_handle
            // and g_src_* intact so Katanga never sees the handle change.
        } else {
            release_shared(api);
            g_src_width = 0; g_src_height = 0; g_src_format = SRC_FORMAT_UNSET;
        }
        if (g_d3d9_shared_tex) { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = nullptr; }
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
