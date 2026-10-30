#pragma once

// Precompiled header for the Xbox UWP smoke test.
//
// Kept deliberately small: WINAPI_FAMILY is already set to WINAPI_FAMILY_APP by the
// UWP project configuration (AppContainerApplication=true), so including windows.h
// here only exposes the AppContainer-legal subset of the Win32 surface. If a header
// below ever needs a desktop-only API, the build fails here rather than at runtime
// on the console.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// Include set taken from Microsoft's Xbox ATG UWP samples (SimpleHDR_UWP/pch.h), because
// these exact choices matter:
//   * <dxgi1_6.h>, not <dxgi.h>: the latter only declares up to IDXGISwapChain/IDXGIFactory1,
//     so IDXGISwapChain1 and IDXGIFactory2 are missing and every ComPtr<IDXGISwapChain1>
//     collapses with C2923 -> C2512 -> C2039.
//   * <wrl.h> guarded by "warning 4467 disabled": C++/CX (/ZW) otherwise emits that warning
//     through the WRL headers.
//   * <d3d11_3.h> pulls in <d3d11.h> and is what the samples compile against.
#pragma warning(push)
#pragma warning(disable : 4467)
#include <wrl.h>
#pragma warning(pop)

#include <d3d11_3.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

// C++/CX
#include <collection.h>
#include <ppltasks.h>

// WinRT. Included explicitly rather than relied upon transitively: header ordering in
// the WinRT projections is not something to gamble the first console build on.
#include <windows.foundation.h>
#include <windows.applicationmodel.h>
#include <windows.applicationmodel.activation.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.graphics.display.h>
#include <windows.system.h>

namespace DX {
    using Microsoft::WRL::ComPtr;
}
