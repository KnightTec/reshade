/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "d3d9_device.hpp"
#include "d3d9_resource.hpp"
#include "d3d9_swapchain.hpp"
#include "dll_log.hpp" // Include late to get HRESULT log overloads
#include "com_utils.hpp"

Direct3DSwapChain9::Direct3DSwapChain9(Direct3DDevice9 *device, IDirect3DSwapChain9   *original) :
	swapchain_impl(device, original),
	_extended_interface(0),
	_device(device)
{
	assert(_orig != nullptr && _device != nullptr);
}
Direct3DSwapChain9::Direct3DSwapChain9(Direct3DDevice9 *device, IDirect3DSwapChain9Ex *original) :
	swapchain_impl(device, original),
	_extended_interface(1),
	_device(device)
{
	assert(_orig != nullptr && _device != nullptr);
}

#if RESHADE_ADDON
void Direct3DSwapChain9::reset_back_buffers(IDirect3DSwapChain9 *orig)
{
	D3DPRESENT_PARAMETERS pp = {};
	orig->GetPresentParameters(&pp);

	for (UINT i = 0; i < pp.BackBufferCount; ++i)
	{
		com_ptr<IDirect3DSurface9> surface;
		if (FAILED(orig->GetBackBuffer(i, D3DBACKBUFFER_TYPE_MONO, &surface)))
			continue;

		// Do not destroy if application is still holding a reference (it will later get destroyed via 'Direct3DSurface9::Release' in that case)
		if (surface.ref_count() > 1)
			continue;

		const auto surface_proxy = get_private_pointer_d3d9<Direct3DSurface9>(surface.get());
		if (surface_proxy != nullptr)
		{
			delete surface_proxy;
			surface->SetPrivateData(__uuidof(Direct3DSurface9), nullptr, 0, 0);
		}
	}
}
#endif

bool Direct3DSwapChain9::is_presenting_entire_surface(const RECT *source_rect, HWND hwnd)
{
	if (source_rect == nullptr || hwnd == nullptr)
		return true;

	RECT window_rect = {};
	GetClientRect(hwnd, &window_rect);
	return source_rect->left == window_rect.left && source_rect->top == window_rect.top &&
	       source_rect->right == window_rect.right && source_rect->bottom == window_rect.bottom;
}

bool Direct3DSwapChain9::check_and_upgrade_interface(REFIID riid)
{
	if (riid == __uuidof(this) ||
		riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDirect3DSwapChain9))
		return true;
	if (riid != __uuidof(IDirect3DSwapChain9Ex))
		return false;

	if (!_extended_interface)
	{
		IDirect3DSwapChain9Ex *new_interface = nullptr;
		if (FAILED(_orig->QueryInterface(IID_PPV_ARGS(&new_interface))))
			return false;
#if RESHADE_VERBOSE_LOG
		LOG(DEBUG) << "Upgrading IDirect3DSwapChain9 object " << this << " to IDirect3DSwapChain9Ex.";
#endif
		_orig->Release();
		_orig = new_interface;
		_extended_interface = true;
	}

	return true;
}

HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (check_and_upgrade_interface(riid))
	{
		AddRef();
		*ppvObj = this;
		return S_OK;
	}

	return _orig->QueryInterface(riid, ppvObj);
}
ULONG   STDMETHODCALLTYPE Direct3DSwapChain9::AddRef()
{
	_orig->AddRef();
	return InterlockedIncrement(&_ref);
}
ULONG   STDMETHODCALLTYPE Direct3DSwapChain9::Release()
{
	const ULONG ref = InterlockedDecrement(&_ref);
	if (ref != 0)
	{
		_orig->Release();
		return ref;
	}

	const auto it = std::find(_device->_additional_swapchains.begin(), _device->_additional_swapchains.end(), this);
	if (it != _device->_additional_swapchains.end())
	{
		_device->_additional_swapchains.erase(it);
		_device->Release(); // Remove the reference that was added in 'Direct3DDevice9::CreateAdditionalSwapChain'
	}

	const auto orig = _orig;
	const bool extended_interface = _extended_interface;
#if RESHADE_VERBOSE_LOG
	LOG(DEBUG) << "Destroying " << "IDirect3DSwapChain9" << (extended_interface ? "Ex" : "") << " object " << this << " (" << orig << ").";
#endif
	delete this;

#if RESHADE_ADDON
	// Destroy back buffer resources after the swap chain implementation, since the latter potentially still had a reference to them
	reset_back_buffers(orig);
#endif

	// Only release internal reference after the effect runtime has been destroyed, so any references it held are cleaned up at this point
	const ULONG ref_orig = orig->Release();
	if (ref_orig != 0) // Verify internal reference count
		LOG(WARN) << "Reference count for " << "IDirect3DSwapChain9" << (extended_interface ? "Ex" : "") << " object " << this << " (" << orig << ") is inconsistent (" << ref_orig << ").";
	return 0;
}

HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
{
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::present>(
		_device,
		this,
		reinterpret_cast<const reshade::api::rect *>(pSourceRect),
		reinterpret_cast<const reshade::api::rect *>(pDestRect),
		pDirtyRegion != nullptr ? pDirtyRegion->rdh.nCount : 0,
		pDirtyRegion != nullptr ? reinterpret_cast<const reshade::api::rect *>(pDirtyRegion->Buffer) : nullptr);
#endif

	// Only call into the effect runtime if the entire surface is presented, to avoid partial updates messing up effects and the GUI
	if (is_presenting_entire_surface(pSourceRect, hDestWindowOverride))
		swapchain_impl::on_present();

	return _orig->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetFrontBufferData(IDirect3DSurface9 *pDestSurface)
{
#if RESHADE_ADDON
	pDestSurface = reshade::d3d9::to_orig(pDestSurface);
#endif

	return _orig->GetFrontBufferData(pDestSurface);
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer)
{
	const HRESULT hr = _orig->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
		reshade::d3d9::replace_with_resource_proxy<Direct3DSurface9>(_device, ppBackBuffer);
#endif

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus)
{
	return _orig->GetRasterStatus(pRasterStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetDisplayMode(D3DDISPLAYMODE *pMode)
{
	return _orig->GetDisplayMode(pMode);
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetDevice(IDirect3DDevice9 **ppDevice)
{
	if (ppDevice == nullptr)
		return D3DERR_INVALIDCALL;

	_device->AddRef();
	*ppDevice = _device;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
	return _orig->GetPresentParameters(pPresentationParameters);
}

HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetLastPresentCount(UINT *pLastPresentCount)
{
	assert(_extended_interface);
	return static_cast<IDirect3DSwapChain9Ex *>(_orig)->GetLastPresentCount(pLastPresentCount);
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics)
{
	assert(_extended_interface);
	return static_cast<IDirect3DSwapChain9Ex *>(_orig)->GetPresentStats(pPresentationStatistics);
}
HRESULT STDMETHODCALLTYPE Direct3DSwapChain9::GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation)
{
	assert(_extended_interface);
	return static_cast<IDirect3DSwapChain9Ex *>(_orig)->GetDisplayModeEx(pMode, pRotation);
}
