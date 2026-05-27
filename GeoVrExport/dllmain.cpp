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


static IDXGIKeyedMutex* sharedTextureMutex;
static void* sharedTexture;

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
		error << "GeoVrExport: Could not create shared texture " << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	result = stereoSharedBuffer->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&sharedTextureMutex);
	if (FAILED(result)) {
		std::stringstream error;
		error << "GeoVrExport: Could not get IDXGIKeyedMutex " << uint64_t(result);
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
		return;
	}

	result = tempResource->GetSharedHandle(&sharedHandle);

	if (FAILED(result)) {
		std::stringstream error;
		error << "GeoVrExport: Could not create shared handle " << uint64_t(result);
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}
	else {
		reshade::log::message(reshade::log::level::info, "GeoVrExport: D3D11 ready");
	}

	HANDLE katangaFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(sharedHandle), L"Local\\KatangaMappedFile");
	if (katangaFile == NULL)
	{
		std::stringstream error;
		error << "GeoVrExport: Could not create file mapping object " << GetLastError();
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
		error << "GeoVrExport: Could not create map view of file " << GetLastError();
		reshade::log::message(reshade::log::level::error, error.str().c_str());
		return;
	}

	*sharedResourceHandle = sharedHandle;
	tempResource->Release();
	sharedTexture = stereoSharedBuffer;
}

reshade::api::effect_texture_variable get_vr_texture(reshade::api::effect_runtime* runtime) {
	auto vrTexture = runtime->find_texture_variable("3DToElse.fx", "V__texTOT");
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

			if (sharedTextureMutex != NULL)
				sharedTextureMutex->AcquireSync(0, 50);

			runtime->get_command_queue()->get_immediate_command_list()->copy_resource(src, dst);
			runtime->get_command_queue()->flush_immediate_command_list();

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
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
