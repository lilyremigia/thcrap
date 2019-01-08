/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Direct3D 8/9 detouring helper implementation.
  */

#include <thcrap.h>
#include <minid3d.h>
#include <vector>

/// Direct3D API
/// ------------
#define DIRECT3DCREATE_PARAMS \
	UINT SDKVersion

#define D3D_CREATEDEVICE_PARAMS \
	IDirect3D* that, \
	UINT Adapter, \
	D3DDEVTYPE DeviceType, \
	HWND hFocusWindow, \
	DWORD BehaviourFlags, \
	D3DPRESENT_PARAMETERS* pPresentationParameters, \
	void**** ppReturnedDeviceInterface

#define DIRECT3DCREATE(name) void*** __stdcall name(DIRECT3DCREATE_PARAMS)
#define D3D_CREATEDEVICE(name) HRESULT __stdcall name(D3D_CREATEDEVICE_PARAMS)

typedef DIRECT3DCREATE(Direct3DCreate_type);
typedef D3D_CREATEDEVICE(d3d_CreateDevice_type);
/// ------------

// "Direct3D Device Detour" :P
HRESULT __stdcall d3ddd_CreateDevice(
	d3d_CreateDevice_type *c_orig, const std::vector<vtable_detour_t >& detours,
	D3D_CREATEDEVICE_PARAMS
)
{
	auto ret = c_orig(
		that, Adapter, DeviceType, hFocusWindow, BehaviourFlags,
		pPresentationParameters, ppReturnedDeviceInterface
	);
	if(ret == D3D_OK && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
		vtable_detour(**ppReturnedDeviceInterface, detours.data(), detours.size());
	}
	return ret;
}

void*** __stdcall d3ddd_Direct3DCreate(
	Direct3DCreate_type *c_orig, size_t vtable_index,
	d3d_CreateDevice_type *cd_new, d3d_CreateDevice_type **cd_old,
	DIRECT3DCREATE_PARAMS
)
{
	if(!c_orig) {
		return nullptr;
	}
	auto ret = c_orig(SDKVersion);
	if(ret) {
		vtable_detour_t my[] = {
			{ vtable_index, cd_new, (void**)cd_old }
		};
		vtable_detour(*ret, my, elementsof(my));
	}
	return ret;
}

int d3d_device_detour(
	const char* dll_name, const char* func_name,
	Direct3DCreate_type *c_new, Direct3DCreate_type **c_orig,
	std::vector<vtable_detour_t> &detours, 
	vtable_detour_t *new_det, size_t new_det_count
)
{
	assert(new_det);
	assert(new_det_count >= 1);
	if(*c_orig == nullptr) {
		auto hModule = GetModuleHandleA(dll_name);
		if(!hModule) {
			return 0;
		}
		*(FARPROC *)c_orig = GetProcAddress(hModule, func_name);
		detour_chain(dll_name, 1,
			func_name, c_new, &c_orig,
			nullptr
		);
	}
	detours.reserve(detours.size() + new_det_count);
	for(decltype(new_det_count) i = 0; i < new_det_count; i++) {
		detours.emplace_back(new_det[i]);
	}
	return new_det_count;
}

#define D3D_DEVICE_DETOUR(ver, vtable_index) \
	std::vector<vtable_detour_t> detours_d3dd##ver; \
	Direct3DCreate_type *chain_Direct3DCreate##ver; \
	d3d_CreateDevice_type *chain_d3d_CreateDevice##ver; \
	\
	D3D_CREATEDEVICE(d3ddd_CreateDevice##ver) \
	{ \
		return d3ddd_CreateDevice( \
			chain_d3d_CreateDevice##ver, detours_d3dd##ver, \
			that, Adapter, DeviceType, hFocusWindow, BehaviourFlags, \
			pPresentationParameters, ppReturnedDeviceInterface \
		); \
	} \
	\
	DIRECT3DCREATE(d3ddd_Direct3DCreate##ver) \
	{ \
		return d3ddd_Direct3DCreate( \
			chain_Direct3DCreate##ver, vtable_index, \
			d3ddd_CreateDevice##ver, &chain_d3d_CreateDevice##ver, \
			SDKVersion \
		); \
	} \
	\
	extern "C" THCRAP_API void d3d##ver##_device_detour( \
		vtable_detour_t *det, size_t det_count \
	) \
	{ \
		d3d_device_detour( \
			"d3d" #ver ".dll", "Direct3DCreate" #ver, \
			d3ddd_Direct3DCreate##ver, &chain_Direct3DCreate##ver, \
			detours_d3dd##ver, det, det_count \
		); \
	}

D3D_DEVICE_DETOUR(8, 15);
D3D_DEVICE_DETOUR(9, 16);
