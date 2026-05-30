#include "pch.h"
#include "reshade.hpp"
#include <dxgi1_6.h>
#include <d3d9.h>      // D3D9 bridge (IDirect3DDevice9Ex shared textures)
#include <d3d10_1.h>   // D3D10 bridge
#include <d3d11.h>
#include <d3d12.h>
#include <iostream>
#include <cstdint>
#include <array>
#include <sstream>

extern "C" __declspec(dllexport) const char* NAME = "GeoVrExport";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Export Geo3D stereo buffers to KatangaVR and Artum's VR Viewer.";


static IDXGIKeyedMutex* sharedTextureMutex;
static void* sharedTexture;

// ── D3D9 / D3D10 bridge state ───────────────────────────────────────────────────
// Geo3D's D3D9/D3D10 games render stereo (frame-sequential), 3DToElse reconstructs the
// full SBS into texTOT, and we must hand that to Katanga/VRScreenCap as a D3D11 shared
// handle. D3D9 and D3D10 textures can't be opened directly by a D3D11/Vulkan consumer,
// so we create a shared surface on the game's device, then re-open the SAME GPU memory
// on a standalone D3D11 device and write THAT D3D11 shared handle to KatangaMappedFile.
static ID3D11Device*        g_d3d11_device      = nullptr; // standalone bridge device
static ID3D11DeviceContext* g_d3d11_context     = nullptr;
static IDirect3DTexture9*   g_d3d9_shared_tex   = nullptr; // keeps D3D9 texture alive (sharedTexture = its surface)
static void*                sharedTexture_D3D11 = nullptr; // D3D11 tex Katanga reads (D3D9 bridge path)

// ── D3D9 CPU-staging fallback (non-Ex devices) ──────────────────────────────────
// Legacy D3D9 games (e.g. Dragon Age Origins) create a plain IDirect3DDevice9 via
// Direct3DCreate9, which CANNOT create shared textures (that needs Direct3DCreate9Ex).
// D3D9 also has no cross-device shared-surface mechanism, so we can't bridge on the GPU.
// Fallback: route the SBS pixels through system memory at FULL resolution, using a
// double-buffered (ping-pong) readback to hide the latency so the game never stalls:
//   each frame: StretchRect texTOT -> RT[cur] (GPU snapshot, no wait), then
//   GetRenderTargetData(RT[prev] -> sysmem[prev]) reads the ALREADY-FINISHED previous
//   frame (no GPU drain wait), upload that to the D3D11 SHARED texture, swap cur/prev.
// One frame of extra latency (imperceptible), no pipeline stall, full resolution kept.
// Engaged only when QueryInterface(IDirect3DDevice9Ex) fails; D3D9Ex games keep the fast path.
static bool                 g_d3d9_staging      = false;   // true when CPU-staging path is active
static IDirect3DDevice9*    g_d3d9_game_dev     = nullptr; // game device (not owned, no ref)
static IDirect3DTexture9*   g_d3d9_rt_tex[2]    = { nullptr, nullptr }; // DEFAULT RT snapshot textures
static IDirect3DSurface9*   g_d3d9_rt_surf[2]   = { nullptr, nullptr }; // their level-0 surfaces (StretchRect dst)
static IDirect3DSurface9*   g_d3d9_sysmem[2]    = { nullptr, nullptr }; // SYSTEMMEM readback targets
static int                  g_d3d9_pp           = 0;       // ping-pong index (current)
static bool                 g_d3d9_pp_primed    = false;   // true once prev buffer holds a valid frame
static UINT                 g_d3d9_w            = 0;
static UINT                 g_d3d9_h            = 0;
static D3DFORMAT            g_d3d9_fmt          = D3DFMT_UNKNOWN;

// ── D3D9 GPU-side share (preferred, no CPU readback) ────────────────────────────
// Even when the game's raw device is plain (non-Ex), ReShade's own device abstraction
// can create a SHARED resource on the same device that backs texTOT (ReShade's D3D9
// runtime supports shared resources via its internal Device9Ex). We create that shared
// resource through ReShade's create_resource(resource_flags::shared), open its handle on
// the standalone D3D11 device for Katanga, then each frame do a pure GPU-side
// copy_resource(texTOT -> shared) — identical to the D3D11/D3D12 fast path, zero readback.
// Engaged whenever device->check_capability(shared_resource) is true; otherwise we fall
// through to the CPU-staging path below so nothing regresses on runtimes that can't share.
static reshade::api::resource g_d3d9_gpu_shared = { 0 };       // ReShade shared resource (copy dst)
static reshade::api::device*  g_d3d9_gpu_dev    = nullptr;     // device that owns it (for destroy_resource)

// Copy-completion fence (D3D12 path). The D3D12 shared resource uses
// ALLOW_SIMULTANEOUS_ACCESS with no keyed mutex, and VRScreenCap samples it live
// without acquiring on its side, so without this the consumer can read a half-written
// copy_resource (slight flicker, worse under GPU load). A queue-ordered ID3D12Fence
// Signal + bounded host wait makes the copy GPU-complete before the next frame
// overwrites it. Scoped to the copy, not a full wait_idle() drain. Best-effort.
static ID3D12Fence* g_d3d12_copy_fence  = nullptr;
static HANDLE       g_d3d12_fence_event = nullptr;
static UINT64       g_d3d12_fence_value = 0;
static bool         g_geo_is_d3d12      = false; // true once the D3D12 path is active

// Write a (legacy KMT) shared handle to KatangaMappedFile so VRScreenCap/Katanga can
// open it. Mirrors the inline logic in share_d3d11_texture/share_d3d12_texture; used by
// the D3D9/D3D10 bridge paths to avoid duplicating the mapping boilerplate.
static void write_katanga_handle(HANDLE sharedHandle)
{
	HANDLE katangaFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(sharedHandle), L"Local\\KatangaMappedFile");
	if (katangaFile == NULL) {
		std::stringstream error; error << "Could not create file mapping object " << GetLastError();
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}
	HANDLE* sharedResourceHandle = (HANDLE*)MapViewOfFile(katangaFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(sharedHandle));
	if (sharedResourceHandle == NULL) {
		std::stringstream error; error << "Could not create map view of file " << GetLastError();
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}
	*sharedResourceHandle = sharedHandle;
}

// Create the standalone D3D11 bridge device, bypassing ReShade's D3D11CreateDevice hook
// (a ReShade-proxied device breaks cross-API shared-resource ops). Created once, reused.
static bool ensure_d3d11_device()
{
	if (g_d3d11_device) return true;
	typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(
		IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
		const D3D_FEATURE_LEVEL*, UINT, UINT,
		ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
	HMODULE d3d11_dll = GetModuleHandleA("d3d11.dll");
	if (!d3d11_dll) d3d11_dll = LoadLibraryA("d3d11.dll");
	if (!d3d11_dll) return false;
	auto pfn = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(d3d11_dll, "D3D11CreateDevice"));
	if (!pfn) return false;
	ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(pfn(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
		return false;
	g_d3d11_device = dev; g_d3d11_context = ctx;
	return g_d3d11_device != nullptr;
}

// Open a shared handle (legacy KMT from a D3D9/D3D10 SHARED surface) on the standalone
// D3D11 device and publish its D3D11 KMT handle to Katanga.
static bool open_on_d3d11(HANDLE h)
{
	if (!ensure_d3d11_device()) return false;
	ID3D11Texture2D* tex = nullptr;
	if (FAILED(g_d3d11_device->OpenSharedResource(h, IID_PPV_ARGS(&tex)))) return false;

	IDXGIKeyedMutex* mutex = nullptr;
	tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex));
	IDXGIResource* r = nullptr; HANDLE leg = nullptr;
	if (SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
	if (!leg) { tex->Release(); if (mutex) mutex->Release(); return false; }

	write_katanga_handle(leg);
	sharedTextureMutex  = mutex;        // may be NULL (plain SHARED) — copy still works
	sharedTexture_D3D11 = tex;
	sharedTexture       = tex;
	return true;
}

// Map the handful of D3D9 back-buffer formats we expect (Geo SBS is A8R8G8B8/X8R8G8B8)
// to their DXGI equivalents for the D3D11 staging texture Katanga reads.
static DXGI_FORMAT d3d9_to_dxgi(D3DFORMAT f)
{
	switch (f) {
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM; // ARGB8 == BGRA8 byte order
	case D3DFMT_A2B10G10R10: return DXGI_FORMAT_R10G10B10A2_UNORM;
	default:               return DXGI_FORMAT_B8G8R8A8_UNORM; // safe default for 32-bit RT
	}
}

// Create a D3D11 SHARED, default-usage texture on the standalone bridge device that we
// can UpdateSubresource into, and publish its legacy shared handle to Katanga.
static ID3D11Texture2D* create_d3d11_staging_shared(UINT w, UINT h, DXGI_FORMAT fmt)
{
	if (!ensure_d3d11_device()) return nullptr;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = fmt; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;                 // UpdateSubresource target (not MAPpable, that's fine)
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;      // plain SHARED -> legacy handle for Katanga
	ID3D11Texture2D* tex = nullptr;
	if (FAILED(g_d3d11_device->CreateTexture2D(&td, nullptr, &tex))) return nullptr;
	IDXGIResource* r = nullptr; HANDLE leg = nullptr;
	if (SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
	if (!leg) { tex->Release(); return nullptr; }
	write_katanga_handle(leg);
	return tex;
}

// D3D9 bridge: create a shared D3D9 render-target texture (IDirect3DDevice9Ex), then
// open the SAME GPU memory on the standalone D3D11 device for Katanga. The game copies
// into the D3D9 surface; Katanga reads it as a D3D11 texture (identical memory).
void share_d3d9_texture(reshade::api::effect_runtime* runtime, reshade::api::resource src_res,
                        IDirect3DTexture9* texture, IDirect3DDevice9* device)
{
	// Clean up any previous share (mirror the other share_* functions' release order).
	if (g_d3d9_gpu_shared.handle != 0 && g_d3d9_gpu_dev != nullptr) { g_d3d9_gpu_dev->destroy_resource(g_d3d9_gpu_shared); }
	g_d3d9_gpu_shared = { 0 }; g_d3d9_gpu_dev = nullptr;
	if (sharedTextureMutex != NULL) { sharedTextureMutex->Release(); sharedTextureMutex = NULL; }
	if (sharedTexture_D3D11 != NULL) { ((ID3D11Texture2D*)sharedTexture_D3D11)->Release(); sharedTexture_D3D11 = NULL; }
	if (sharedTexture != NULL) { ((IUnknown*)sharedTexture)->Release(); sharedTexture = NULL; }
	if (g_d3d9_shared_tex != NULL) { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = NULL; }
	for (int i = 0; i < 2; ++i) {
		if (g_d3d9_sysmem[i])  { g_d3d9_sysmem[i]->Release();  g_d3d9_sysmem[i]  = nullptr; }
		if (g_d3d9_rt_surf[i]) { g_d3d9_rt_surf[i]->Release(); g_d3d9_rt_surf[i] = nullptr; }
		if (g_d3d9_rt_tex[i])  { g_d3d9_rt_tex[i]->Release();  g_d3d9_rt_tex[i]  = nullptr; }
	}
	g_d3d9_staging = false; g_d3d9_game_dev = nullptr; g_d3d9_pp = 0; g_d3d9_pp_primed = false;

	// ── Preferred: GPU-side share via ReShade's own device (no CPU readback) ──────
	// Works for both Ex and non-Ex game devices: ReShade's D3D9 runtime can create a
	// SHARED resource on the same device that backs texTOT. We copy texTOT into it on
	// the GPU every frame and Katanga reads the same memory through D3D11.
	{
		reshade::api::device* dev_rs = runtime->get_device();
		if (dev_rs->check_capability(reshade::api::device_caps::shared_resource)) {
			reshade::api::resource_desc sd = dev_rs->get_resource_desc(src_res);
			reshade::api::resource_desc dd(
				sd.texture.width, sd.texture.height, 1, 1, sd.texture.format, 1,
				reshade::api::memory_heap::gpu_only,
				reshade::api::resource_usage::render_target | reshade::api::resource_usage::copy_dest | reshade::api::resource_usage::shader_resource,
				reshade::api::resource_flags::shared);
			reshade::api::resource out_res = { 0 };
			void* shared_handle = nullptr;
			if (dev_rs->create_resource(dd, nullptr, reshade::api::resource_usage::copy_dest, &out_res, &shared_handle)
			    && out_res.handle != 0 && shared_handle != nullptr
			    && open_on_d3d11(reinterpret_cast<HANDLE>(shared_handle))) {
				// open_on_d3d11 set sharedTexture/sharedTexture_D3D11 to the D3D11 view Katanga
				// reads; the copy destination is the ReShade resource, so neutralise the generic
				// sharedTexture copy branch (sharedTexture_D3D11 still holds the D3D11 ref).
				sharedTexture     = nullptr;
				g_d3d9_gpu_shared = out_res;
				g_d3d9_gpu_dev    = dev_rs;
				reshade::log::message(reshade::log::level::info, "GeoVrExport: D3D9 ready (GPU shared via ReShade, non-Ex device)");
				return;
			}
			// Setup failed — undo any partial state and fall through to the legacy paths.
			if (out_res.handle != 0) dev_rs->destroy_resource(out_res);
			if (sharedTexture_D3D11) { ((ID3D11Texture2D*)sharedTexture_D3D11)->Release(); sharedTexture_D3D11 = nullptr; }
			if (sharedTextureMutex)  { sharedTextureMutex->Release(); sharedTextureMutex = NULL; }
			sharedTexture = nullptr;
			reshade::log::message(reshade::log::level::warning, "GeoVrExport: D3D9 GPU share unavailable, falling back");
		}
	}

	IDirect3DDevice9Ex* devEx = nullptr;
	if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&devEx)))) {
		// ── CPU-staging fallback for plain (non-Ex) D3D9 devices, FULL RES, no stall ──
		// Double-buffered: StretchRect texTOT -> RT[cur] (GPU snapshot, no wait), then
		// read back RT[prev] (already finished) and upload to the D3D11 SHARED texture.
		D3DSURFACE_DESC sd; texture->GetLevelDesc(0, &sd);
		bool ok = true;
		for (int i = 0; i < 2 && ok; ++i) {
			// DEFAULT render-target texture: StretchRect destination (GPU-side snapshot).
			if (FAILED(device->CreateTexture(sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET,
			                                 sd.Format, D3DPOOL_DEFAULT, &g_d3d9_rt_tex[i], nullptr))) { ok = false; break; }
			g_d3d9_rt_tex[i]->GetSurfaceLevel(0, &g_d3d9_rt_surf[i]);
			// SYSTEMMEM surface: GetRenderTargetData destination (CPU-readable).
			if (FAILED(device->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format,
			                                 D3DPOOL_SYSTEMMEM, &g_d3d9_sysmem[i], nullptr))) { ok = false; break; }
		}
		ID3D11Texture2D* d11 = ok ? create_d3d11_staging_shared(sd.Width, sd.Height, d3d9_to_dxgi(sd.Format)) : nullptr;
		if (!ok || !d11) {
			for (int i = 0; i < 2; ++i) {
				if (g_d3d9_sysmem[i])  { g_d3d9_sysmem[i]->Release();  g_d3d9_sysmem[i]  = nullptr; }
				if (g_d3d9_rt_surf[i]) { g_d3d9_rt_surf[i]->Release(); g_d3d9_rt_surf[i] = nullptr; }
				if (g_d3d9_rt_tex[i])  { g_d3d9_rt_tex[i]->Release();  g_d3d9_rt_tex[i]  = nullptr; }
			}
			reshade::log::message(reshade::log::level::error, "GeoVrExport: D3D9 staging setup failed");
			return;
		}
		sharedTexture_D3D11 = d11;     // Katanga reads this
		g_d3d9_game_dev     = device;  // for StretchRect / GetRenderTargetData (not owned)
		g_d3d9_w = sd.Width; g_d3d9_h = sd.Height; g_d3d9_fmt = sd.Format;
		g_d3d9_pp = 0; g_d3d9_pp_primed = false;
		g_d3d9_staging = true;
		// NOTE: sharedTexture stays NULL — the staging copy path uses g_d3d9_* instead.
		reshade::log::message(reshade::log::level::info, "GeoVrExport: D3D9 ready (CPU staging full-res, non-Ex device)");
		return;
	}
	D3DSURFACE_DESC d; texture->GetLevelDesc(0, &d);
	IDirect3DTexture9* s9 = nullptr; HANDLE h9 = nullptr;
	HRESULT hr = devEx->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &s9, &h9);
	devEx->Release();
	if (FAILED(hr) || !h9) { if (s9) s9->Release(); reshade::log::message(reshade::log::level::error, "D3D9 CreateTexture failed"); return; }

	if (!open_on_d3d11(h9)) { s9->Release(); reshade::log::message(reshade::log::level::error, "D3D9 bridge failed"); return; }

	// ReShade D3D9 resource handles are IDirect3DSurface9* — copy dst must be surface level 0.
	IDirect3DSurface9* surf = nullptr;
	s9->GetSurfaceLevel(0, &surf);
	g_d3d9_shared_tex = s9;   // hold ref to keep the texture alive
	sharedTexture     = surf; // copy_resource dst (same-API copy, valid)
	reshade::log::message(reshade::log::level::info, "GeoVrExport: D3D9 ready (D3D9Ex shared)");
}

// D3D10 bridge: D3D10 shares natively with D3D11-class consumers, so a plain SHARED
// texture's legacy handle can be handed straight to Katanga (no re-open needed).
void share_d3d10_texture(ID3D10Texture2D* texture, ID3D10Device* device)
{
	if (sharedTextureMutex != NULL) { sharedTextureMutex->Release(); sharedTextureMutex = NULL; }
	if (sharedTexture_D3D11 != NULL) { ((ID3D11Texture2D*)sharedTexture_D3D11)->Release(); sharedTexture_D3D11 = NULL; }
	if (sharedTexture != NULL) { ((IUnknown*)sharedTexture)->Release(); sharedTexture = NULL; }

	D3D10_TEXTURE2D_DESC d; texture->GetDesc(&d);
	d.BindFlags      = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
	d.CPUAccessFlags = 0;
	d.Usage          = D3D10_USAGE_DEFAULT;
	d.MiscFlags      = D3D10_RESOURCE_MISC_SHARED; // plain SHARED — KEYEDMUTEX breaks GetSharedHandle

	ID3D10Texture2D* shared = nullptr;
	if (FAILED(device->CreateTexture2D(&d, nullptr, &shared))) {
		reshade::log::message(reshade::log::level::error, "D3D10 CreateTexture2D failed"); return;
	}
	IDXGIResource* r = nullptr; HANDLE leg = nullptr;
	if (SUCCEEDED(shared->QueryInterface(IID_PPV_ARGS(&r)))) { r->GetSharedHandle(&leg); r->Release(); }
	if (!leg) { shared->Release(); reshade::log::message(reshade::log::level::error, "D3D10 GetSharedHandle failed"); return; }

	write_katanga_handle(leg);
	sharedTextureMutex  = NULL;   // plain SHARED, D3D10 copies are synchronous (no mutex)
	sharedTexture_D3D11 = NULL;
	sharedTexture       = shared;
	reshade::log::message(reshade::log::level::info, "GeoVrExport: D3D10 ready");
}

void share_d3d11_texture(ID3D11Texture2D* texture, ID3D11Device* device)
{
	D3D11_TEXTURE2D_DESC texDesc;
	texture->GetDesc(&texDesc);
	texDesc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	if (sharedTextureMutex != NULL) {
		sharedTextureMutex->Release();
		sharedTextureMutex = NULL;
	}

	if (sharedTexture != NULL) {
		((ID3D11Texture2D*)sharedTexture)->Release();
		sharedTexture = NULL;
	}

	ID3D11Texture2D* stereoSharedBuffer;
	HRESULT result = device->CreateTexture2D(&texDesc, NULL, &stereoSharedBuffer);

	if (FAILED(result)) {
		std::stringstream error;
		error << "Could not create shared texture" << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	result = stereoSharedBuffer->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&sharedTextureMutex);
	if (FAILED(result)) {
		std::stringstream error;
		error << "Could not get IDXGIKeyedMutex" << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	HANDLE sharedHandle = nullptr;
	IDXGIResource* tempResource = NULL;
	result = stereoSharedBuffer->QueryInterface(__uuidof(IDXGIResource), (void**)&tempResource);

	if (FAILED(result)) {
		std::stringstream error;
		error << "Could not get IDXGIResource" << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	result = tempResource->GetSharedHandle(&sharedHandle);

	if (FAILED(result)) {
		std::stringstream error;
		error << "Could not create shared handle " << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}
	else {
		std::stringstream message;
		message << "Got shared handle " << sharedHandle;
		reshade::log::message(reshade::log::level::info, message.str().c_str());
	}

	HANDLE katangaFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(sharedHandle), L"Local\\KatangaMappedFile");
	if (katangaFile == NULL)
	{
		std::stringstream error;
		error << "Could not create file mapping object " << GetLastError();
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}


	HANDLE* sharedResourceHandle = (HANDLE*)MapViewOfFile(katangaFile,
		FILE_MAP_ALL_ACCESS,
		0,
		0,
		sizeof(sharedHandle));

	if (sharedResourceHandle == NULL)
	{
		std::stringstream error;
		error << "Could not create map view of file " << GetLastError();
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	*sharedResourceHandle = sharedHandle;
	tempResource->Release();
	sharedTexture = stereoSharedBuffer;
}

void share_d3d12_texture(ID3D12Resource* texture, ID3D12Device* device)
{
	D3D12_RESOURCE_DESC texDesc = texture->GetDesc();
	texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	
	D3D12_HEAP_PROPERTIES texHeapProperties = D3D12_HEAP_PROPERTIES{ D3D12_HEAP_TYPE_DEFAULT };

	if (sharedTextureMutex != NULL) {
		sharedTextureMutex->Release();
		sharedTextureMutex = NULL;
	}

	if (sharedTexture != NULL) {
		((ID3D12Resource*)sharedTexture)->Release();
		sharedTexture = NULL;
	}

	ID3D12Resource* stereoSharedBuffer;
	
	HRESULT result = device->CreateCommittedResource(
		&texHeapProperties,
		D3D12_HEAP_FLAG_SHARED,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&stereoSharedBuffer)
	);

	if (FAILED(result)) {
		std::stringstream error;
		error << "Could not create shared stream texture" << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	HANDLE sharedHandle = nullptr;
	result = device->CreateSharedHandle(stereoSharedBuffer, nullptr, GENERIC_ALL, L"DX12VRStream", &sharedHandle);

	if (FAILED(result)) {
		std::stringstream error;
		error << "Could not create shared handle " << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}
	else {
		std::stringstream message;
		message << "Got shared handle " << sharedHandle;
		reshade::log::message(reshade::log::level::info, message.str().c_str());
	}

	HANDLE katangaFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(sharedHandle), L"Local\\KatangaMappedFile");
	if (katangaFile == NULL)
	{
		std::stringstream error;
		error << "Could not create file mapping object " << GetLastError();
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}


	HANDLE* sharedResourceHandle = (HANDLE*)MapViewOfFile(katangaFile,
		FILE_MAP_ALL_ACCESS,
		0,
		0,
		sizeof(sharedHandle));

	if (sharedResourceHandle == NULL)
	{
		std::stringstream error;
		error << "Could not create map view of file " << GetLastError();
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	*sharedResourceHandle = sharedHandle;
	sharedTexture = stereoSharedBuffer;
	g_geo_is_d3d12 = true;

	// Create the copy-completion fence + event (best-effort; copy still works without it).
	if (g_d3d12_copy_fence == nullptr) {
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12_copy_fence)))) {
			g_d3d12_copy_fence = nullptr;
		} else {
			g_d3d12_fence_value = 0;
			if (g_d3d12_fence_event == nullptr)
				g_d3d12_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		}
	}
}

reshade::api::effect_texture_variable get_vr_texture(reshade::api::effect_runtime* runtime) {
	auto vrTexture = runtime->find_texture_variable("3DToElse.fx", "V__texTOT");
	if (vrTexture == NULL) {
		vrTexture = runtime->find_texture_variable("SuperDepth3D_VR+.fx", "V__DoubleTex");
	}
	if (vrTexture == NULL) {
		vrTexture = runtime->find_texture_variable("SuperDepth3D_VR+.fx", "V__SuperDepth3DVR__DoubleTex");
	}
	return vrTexture;
}

reshade::api::resource get_texture_resource(reshade::api::effect_runtime* runtime, reshade::api::effect_texture_variable texture) {
	reshade::api::resource_view vr_view;
	reshade::api::resource_view srgb_vr_view;
	runtime->get_texture_binding(texture, &vr_view, &srgb_vr_view);
	if (vr_view.handle != NULL || srgb_vr_view.handle != NULL) {
		return runtime->get_device()->get_resource_from_view(srgb_vr_view.handle != NULL ? srgb_vr_view : vr_view);
	}
	return reshade::api::resource{0};
}

void export_effects(reshade::api::effect_runtime* runtime)
{
	reshade::log::message(reshade::log::level::info, "Searching vr buffer...");
	auto vrTexture = get_vr_texture(runtime);
	if (vrTexture != NULL)
	{
		reshade::log::message(reshade::log::level::info, "Found VR Buffer, sharing...");
		reshade::api::resource texture = get_texture_resource(runtime, vrTexture);
		
		if (texture.handle != NULL) {
			switch (runtime->get_device()->get_api())
			{
			case reshade::api::device_api::d3d9:
				share_d3d9_texture(runtime, texture, reinterpret_cast<IDirect3DTexture9*>(texture.handle), reinterpret_cast<IDirect3DDevice9*>(runtime->get_device()->get_native()));
				break;
			case reshade::api::device_api::d3d10:
				share_d3d10_texture(reinterpret_cast<ID3D10Texture2D*>(texture.handle), reinterpret_cast<ID3D10Device*>(runtime->get_device()->get_native()));
				break;
			case reshade::api::device_api::d3d11:
				share_d3d11_texture(reinterpret_cast<ID3D11Texture2D*>(texture.handle), reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native()));
				break;
			case reshade::api::device_api::d3d12:
				share_d3d12_texture(reinterpret_cast<ID3D12Resource*>(texture.handle), reinterpret_cast<ID3D12Device*>(runtime->get_device()->get_native()));
				break;
			}
		}
	}
}

void add_copy_command(reshade::api::effect_runtime* runtime, reshade::api::command_list * cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb)
{
	auto vrTexture = get_vr_texture(runtime);
	if (vrTexture != NULL)
	{
		reshade::api::resource src = get_texture_resource(runtime, vrTexture);

		// ── D3D9 GPU-side path (preferred): pure GPU copy texTOT -> ReShade shared ──
		// No readback: same-device copy_resource, identical to the D3D11/D3D12 fast path.
		// Katanga reads the shared memory through the D3D11 texture opened in setup.
		if (g_d3d9_gpu_shared.handle != 0) {
			if (src.handle != NULL) {
				runtime->get_command_queue()->get_immediate_command_list()->copy_resource(src, g_d3d9_gpu_shared);
				runtime->get_command_queue()->flush_immediate_command_list();
			}
			return;
		}

		// ── D3D9 CPU-staging path (non-Ex device), full-res, non-stalling ───────────
		// Ping-pong: StretchRect texTOT -> RT[cur] (GPU snapshot, returns immediately),
		// then read back RT[prev] which the GPU finished a frame ago (no drain wait),
		// upload it to the D3D11 SHARED texture, then swap. One frame of latency, no stall.
		if (g_d3d9_staging) {
			if (src.handle != NULL && g_d3d9_game_dev != NULL && sharedTexture_D3D11 != NULL &&
			    g_d3d9_rt_surf[0] != NULL && g_d3d9_sysmem[0] != NULL) {
				IUnknown* unk = reinterpret_cast<IUnknown*>(src.handle);
				IDirect3DSurface9* srcSurf = nullptr;
				bool surfNeedsRelease = false;
				if (SUCCEEDED(unk->QueryInterface(__uuidof(IDirect3DSurface9), reinterpret_cast<void**>(&srcSurf)))) {
					surfNeedsRelease = true;
				} else {
					IDirect3DTexture9* tex9 = nullptr;
					if (SUCCEEDED(unk->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex9)))) {
						tex9->GetSurfaceLevel(0, &srcSurf);
						surfNeedsRelease = true;
						tex9->Release();
					}
				}
				if (srcSurf != nullptr) {
					const int cur  = g_d3d9_pp;
					const int prev = g_d3d9_pp ^ 1;
					// 1) GPU-side snapshot of THIS frame into RT[cur]. StretchRect returns
					//    without waiting for the CPU; same size/format = fast blit.
					g_d3d9_game_dev->StretchRect(srcSurf, nullptr, g_d3d9_rt_surf[cur], nullptr, D3DTEXF_NONE);
					// 2) Read back the PREVIOUS frame's snapshot (already complete on the GPU,
					//    so no pipeline drain), and publish it to Katanga.
					if (g_d3d9_pp_primed) {
						if (SUCCEEDED(g_d3d9_game_dev->GetRenderTargetData(g_d3d9_rt_surf[prev], g_d3d9_sysmem[prev]))) {
							D3DLOCKED_RECT lr;
							if (SUCCEEDED(g_d3d9_sysmem[prev]->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
								g_d3d11_context->UpdateSubresource(
									reinterpret_cast<ID3D11Texture2D*>(sharedTexture_D3D11),
									0, nullptr, lr.pBits, lr.Pitch, lr.Pitch * g_d3d9_h);
								g_d3d9_sysmem[prev]->UnlockRect();
							}
						}
					}
					g_d3d9_pp = prev;          // swap
					g_d3d9_pp_primed = true;   // prev now holds a valid snapshot
					if (surfNeedsRelease) srcSurf->Release();
				}
			}
			return; // staging handled — skip the shared-texture copy path below
		}

		if (src.handle != NULL && sharedTexture != NULL) {
			reshade::api::resource dst;
			dst.handle = uint64_t(sharedTexture);

			if (sharedTextureMutex != NULL)
				sharedTextureMutex->AcquireSync(0, 50);

			runtime->get_command_queue()->get_immediate_command_list()->copy_resource(src, dst);
			runtime->get_command_queue()->flush_immediate_command_list();

			// D3D12 path: wait until the copy is GPU-complete so VRScreenCap (which samples
			// the shared texture live, no acquire on its side) never reads a half-written
			// copy. Queue-ordered fence Signal + bounded host wait, scoped to the copy.
			// Bounded 8ms timeout prevents any hang if the GPU is saturated. Best-effort.
			if (g_geo_is_d3d12 && g_d3d12_copy_fence != NULL && g_d3d12_fence_event != NULL) {
				ID3D12CommandQueue* q = reinterpret_cast<ID3D12CommandQueue*>(
					static_cast<uintptr_t>(runtime->get_command_queue()->get_native()));
				if (q != NULL) {
					const UINT64 target = ++g_d3d12_fence_value;
					if (SUCCEEDED(q->Signal(g_d3d12_copy_fence, target))) {
						if (g_d3d12_copy_fence->GetCompletedValue() < target) {
							if (SUCCEEDED(g_d3d12_copy_fence->SetEventOnCompletion(target, g_d3d12_fence_event))) {
								WaitForSingleObject(g_d3d12_fence_event, 8);
							}
						}
					}
				}
			}

			if (sharedTextureMutex != NULL)
				sharedTextureMutex->ReleaseSync(0);
		}
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(export_effects);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(add_copy_command);
		break;
	case DLL_PROCESS_DETACH:
		if (g_d3d12_copy_fence)  { g_d3d12_copy_fence->Release(); g_d3d12_copy_fence = nullptr; }
		if (g_d3d12_fence_event) { CloseHandle(g_d3d12_fence_event); g_d3d12_fence_event = nullptr; }
		if (g_d3d9_sysmem[0])    { g_d3d9_sysmem[0]->Release();  g_d3d9_sysmem[0]  = nullptr; }
		if (g_d3d9_sysmem[1])    { g_d3d9_sysmem[1]->Release();  g_d3d9_sysmem[1]  = nullptr; }
		if (g_d3d9_rt_surf[0])   { g_d3d9_rt_surf[0]->Release(); g_d3d9_rt_surf[0] = nullptr; }
		if (g_d3d9_rt_surf[1])   { g_d3d9_rt_surf[1]->Release(); g_d3d9_rt_surf[1] = nullptr; }
		if (g_d3d9_rt_tex[0])    { g_d3d9_rt_tex[0]->Release();  g_d3d9_rt_tex[0]  = nullptr; }
		if (g_d3d9_rt_tex[1])    { g_d3d9_rt_tex[1]->Release();  g_d3d9_rt_tex[1]  = nullptr; }
		if (g_d3d9_shared_tex)   { g_d3d9_shared_tex->Release(); g_d3d9_shared_tex = nullptr; }
		g_d3d9_gpu_shared = { 0 }; g_d3d9_gpu_dev = nullptr; // owned by ReShade's device, destroyed on unregister
		if (sharedTexture_D3D11) { ((ID3D11Texture2D*)sharedTexture_D3D11)->Release(); sharedTexture_D3D11 = nullptr; }
		if (g_d3d11_context)     { g_d3d11_context->Release(); g_d3d11_context = nullptr; }
		if (g_d3d11_device)      { g_d3d11_device->Release(); g_d3d11_device = nullptr; }
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
