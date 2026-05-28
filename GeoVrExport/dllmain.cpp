#include "pch.h"
#include "reshade.hpp"
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>
#include <iostream>
#include <cstdint>
#include <array>
#include <sstream>

extern "C" __declspec(dllexport) const char* NAME = "GeoVrExport";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Export Geo3D / frame-sequential stereo buffers to KatanaVR and VRScreenCap.";


static void* sharedTexture;
static HANDLE  g_katanga_mapping = nullptr;
static DWORD* g_katanga_view    = nullptr;

void share_d3d11_texture(ID3D11Texture2D* texture, ID3D11Device* device)
{
	D3D11_TEXTURE2D_DESC texDesc;
	texture->GetDesc(&texDesc);
	texDesc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	texDesc.CPUAccessFlags = 0;
	// Plain SHARED — SHARED_KEYEDMUTEX causes GetSharedHandle to return null.
	// Katanga reads the handle as UINT via *(PUINT)(pMappedView) — needs valid KMT handle.
	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	if (sharedTexture != NULL) {
		((ID3D11Texture2D*)sharedTexture)->Release();
		sharedTexture = NULL;
	}

	ID3D11Texture2D* stereoSharedBuffer;
	HRESULT result = device->CreateTexture2D(&texDesc, NULL, &stereoSharedBuffer);

	if (FAILED(result)) {
		std::stringstream error;
		error << "GeoVrExport: Could not create shared texture " << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	HANDLE sharedHandle = nullptr;
	IDXGIResource* tempResource = NULL;
	result = stereoSharedBuffer->QueryInterface(__uuidof(IDXGIResource), (void**)&tempResource);

	if (FAILED(result)) {
		std::stringstream error;
		error << "GeoVrExport: Could not get IDXGIResource " << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		stereoSharedBuffer->Release();
		return;
	}

	result = tempResource->GetSharedHandle(&sharedHandle);
	tempResource->Release();

	if (FAILED(result) || sharedHandle == nullptr) {
		std::stringstream error;
		error << "GeoVrExport: Could not get shared handle " << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		stereoSharedBuffer->Release();
		return;
	}

	reshade::log::message(reshade::log::level::info, "GeoVrExport: D3D11 ready");

		// Persistent mapping: stays alive across DLL reload so KatanaVR stays connected.
	// On reload: CreateFileMapping returns existing named mapping, value updated in-place.
	if (!g_katanga_mapping) {
		g_katanga_mapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL,
			PAGE_READWRITE, 0, sizeof(DWORD), L"Local\\KatangaMappedFile");
		if (!g_katanga_mapping) { stereoSharedBuffer->Release(); return; }
		g_katanga_view = (DWORD*)MapViewOfFile(g_katanga_mapping,
			FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DWORD));
	}
	if (g_katanga_view) *g_katanga_view = (DWORD)(uintptr_t)sharedHandle;
	sharedTexture = stereoSharedBuffer;
}

reshade::api::effect_texture_variable get_vr_texture(reshade::api::effect_runtime* runtime) {
	auto vrTexture = runtime->find_texture_variable(nullptr, "V__texTOT"); // nullptr searches all effects regardless of path
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
	auto vrTexture = get_vr_texture(runtime);
	if (vrTexture != NULL)
	{
		reshade::api::resource texture = get_texture_resource(runtime, vrTexture);
		
		if (texture.handle != NULL) {
			switch (runtime->get_device()->get_api())
			{
			case reshade::api::device_api::d3d11:
				share_d3d11_texture(reinterpret_cast<ID3D11Texture2D*>(texture.handle), reinterpret_cast<ID3D11Device*>(runtime->get_device()->get_native()));
				break;
			}
		}
	}
	else {
		reshade::log::message(reshade::log::level::warning, "GeoVrExport: VR buffer not found");
	}
}

void add_copy_command(reshade::api::effect_runtime* runtime, reshade::api::command_list * cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb)
{
	auto vrTexture = get_vr_texture(runtime);
	if (vrTexture != NULL)
	{
		reshade::api::resource src = get_texture_resource(runtime, vrTexture);
		if (src.handle != NULL && sharedTexture != NULL) {
			reshade::api::resource dst;
			dst.handle = uint64_t(sharedTexture);
			// No keyed mutex — plain SHARED flag. D3D11 immediate context copy_resource
			// is synchronous — flush not needed and causes per-frame GPU stalls that
			// break frame-sequential L/R alternation timing.
			runtime->get_command_queue()->get_immediate_command_list()->copy_resource(src, dst);
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
		if (sharedTexture)    { ((ID3D11Texture2D*)sharedTexture)->Release(); sharedTexture = nullptr; }
		if (g_katanga_view)   { UnmapViewOfFile(g_katanga_view);  g_katanga_view   = nullptr; }
		if (g_katanga_mapping){ CloseHandle(g_katanga_mapping);   g_katanga_mapping = nullptr; }
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
