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
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cmath>
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
#include <windows.applicationmodel.activation.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.graphics.display.h>
#include <windows.system.h>

namespace DX {
    using Microsoft::WRL::ComPtr;
}
