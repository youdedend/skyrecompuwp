#include "pch.h"
#include "App.h"

using namespace Platform;
using namespace Microsoft::WRL;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Core;
using namespace Windows::Graphics::Display;
using namespace Windows::System;
using namespace Windows::Foundation;

using namespace SkyRecompUwpSmoke;

namespace {
    inline void ThrowIfFailed(HRESULT hr) {
        if (FAILED(hr)) { throw Exception::CreateException(hr); }
    }
}

// ------------------------------------------------------------------ view + factory

App::App()
    : m_windowClosed(false)
    , m_visible(true)
{
    QueryPerformanceFrequency(&m_qpcFrequency);
    QueryPerformanceCounter(&m_qpcStart);
}

IFrameworkView^ AppSource::CreateView()
{
    return ref new App();
}

void App::Initialize(CoreApplicationView^ applicationView)
{
    applicationView->Activated +=
        ref new TypedEventHandler<CoreApplicationView^, IActivatedEventArgs^>(this, &App::OnActivated);

    CoreApplication::Suspending +=
        ref new EventHandler<SuspendingEventArgs^>(this, &App::OnSuspending);

    CoreApplication::Resuming +=
        ref new EventHandler<Object^>(this, &App::OnResuming);
}

void App::SetWindow(CoreWindow^ window)
{
    window->SizeChanged +=
        ref new TypedEventHandler<CoreWindow^, WindowSizeChangedEventArgs^>(this, &App::OnWindowSizeChanged);

    window->Closed +=
        ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &App::OnWindowClosed);

    window->KeyDown +=
        ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &App::OnKeyDown);

    m_windowBounds = window->Bounds;

    CreateDeviceResources();
    CreateWindowSizeDependentResources();
}

void App::Load(String^ /*entryPoint*/)
{
    // No XAML, no entry-point string to consume.
}

void App::Uninitialize()
{
}

void App::Run()
{
    CoreWindow^ window = CoreWindow::GetForCurrentThread();

    while (!m_windowClosed)
    {
        if (m_visible)
        {
            window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double elapsed = double(now.QuadPart - m_qpcStart.QuadPart) / double(m_qpcFrequency.QuadPart);

            Render(elapsed);

            // Present with vsync. 1 = sync interval.
            HRESULT hr = m_swapChain->Present(1, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            {
                // Nothing sensible to recover for a smoke test: exit and say so.
                OutputDebugStringW(L"[smoke] device removed\n");
                break;
            }
            ThrowIfFailed(hr);

            if (elapsed >= kAutoExitSeconds)
            {
                OutputDebugStringW(L"[smoke] auto exit\n");
                CoreApplication::Exit();
                break;
            }
        }
        else
        {
            window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
        }
    }
}

// ------------------------------------------------------------------ events

void App::OnActivated(CoreApplicationView^ /*sender*/, IActivatedEventArgs^ /*args*/)
{
    CoreWindow::GetForCurrentThread()->Activate();
}

void App::OnSuspending(Object^ /*sender*/, SuspendingEventArgs^ /*args*/)
{
    m_visible = false;
}

void App::OnResuming(Object^ /*sender*/, Object^ /*args*/)
{
    m_visible = true;
}

void App::OnWindowClosed(CoreWindow^ /*sender*/, CoreWindowEventArgs^ /*args*/)
{
    m_windowClosed = true;
}

void App::OnWindowSizeChanged(CoreWindow^ sender, WindowSizeChangedEventArgs^ /*args*/)
{
    m_windowBounds = sender->Bounds;
    CreateWindowSizeDependentResources();
}

void App::OnKeyDown(CoreWindow^ /*sender*/, KeyEventArgs^ args)
{
    // Escape and GoBack both map to "leave" on a pad/keyboard, depending on the OS build.
    if (args->VirtualKey == VirtualKey::Escape || args->VirtualKey == VirtualKey::GoBack)
    {
        m_windowClosed = true;
        CoreApplication::Exit();
    }
}

// ------------------------------------------------------------------ d3d

void App::CreateDeviceResources()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Series X|S exposes D3D feature level 11.0 to UWP apps; 10.0 covers Xbox One.
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, _countof(levels), D3D11_SDK_VERSION,
        m_d3dDevice.GetAddressOf(), &got, m_d3dContext.GetAddressOf());

#if defined(_DEBUG)
    // A dev machine without the Graphics Tools feature has no debug layer.
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG,
            levels, _countof(levels), D3D11_SDK_VERSION,
            m_d3dDevice.GetAddressOf(), &got, m_d3dContext.GetAddressOf());
    }
#endif
    ThrowIfFailed(hr);

    wchar_t msg[128];
    swprintf_s(msg, L"[smoke] D3D11 device created, feature level 0x%x\n", unsigned(got));
    OutputDebugStringW(msg);
}

void App::CreateWindowSizeDependentResources()
{
    if (!m_d3dDevice) { return; }

    ReleaseWindowSizeDependentResources();

    CoreWindow^ window = CoreWindow::GetForCurrentThread();
    if (!window) { return; }

    const float dpi = DisplayInformation::GetForCurrentView()->LogicalDpi;
    const float scale = dpi / 96.0f;

    // A 0x0 buffer is legal for a CoreWindow swap chain (it takes the window size),
    // but being explicit keeps the resize path honest.
    UINT width  = UINT(window->Bounds.Width  * scale + 0.5f);
    UINT height = UINT(window->Bounds.Height * scale + 0.5f);
    if (width == 0)  { width = 1920; }
    if (height == 0) { height = 1080; }

    if (!m_swapChain)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ThrowIfFailed(m_d3dDevice.As(&dxgiDevice));

        ComPtr<IDXGIAdapter> adapter;
        ThrowIfFailed(dxgiDevice->GetAdapter(adapter.GetAddressOf()));

        ComPtr<IDXGIFactory2> factory;
        ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())));

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width              = width;
        desc.Height             = height;
        desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo             = FALSE;
        desc.SampleDesc.Count   = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount        = 2;
        desc.Scaling            = DXGI_SCALING_STRETCH;
        desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;

        ThrowIfFailed(factory->CreateSwapChainForCoreWindow(
            dxgiDevice.Get(), reinterpret_cast<IUnknown*>(window),
            &desc, nullptr, m_swapChain.GetAddressOf()));
    }
    else
    {
        ThrowIfFailed(m_swapChain->ResizeBuffers(
            2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())));
    ThrowIfFailed(m_d3dDevice->CreateRenderTargetView(
        backBuffer.Get(), nullptr, m_renderTargetView.GetAddressOf()));

    // Bind the render target and the full viewport. Forgetting the viewport is the
    // classic "black screen with a correct swap chain".
    ID3D11RenderTargetView* rtvs[] = { m_renderTargetView.Get() };
    m_d3dContext->OMSetRenderTargets(1, rtvs, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = float(width);
    vp.Height   = float(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_d3dContext->RSSetViewports(1, &vp);

    m_windowBounds = window->Bounds;
}

void App::ReleaseWindowSizeDependentResources()
{
    if (m_d3dContext) { m_d3dContext->OMSetRenderTargets(0, nullptr, nullptr); }
    m_renderTargetView.Reset();
}

void App::Render(double elapsedSeconds)
{
    if (!m_renderTargetView) { return; }

    // Animate so a frozen frame is immediately distinguishable from a working one.
    const float t = float(elapsedSeconds);
    const float r = 0.5f + 0.5f * sinf(t * 1.7f);
    const float g = 0.5f + 0.5f * sinf(t * 2.3f + 2.1f);
    const float b = 0.5f + 0.5f * sinf(t * 1.1f + 4.2f);

    const float clear[4] = { r * 0.35f, g * 0.35f, b * 0.45f, 1.0f };
    m_d3dContext->ClearRenderTargetView(m_renderTargetView.Get(), clear);
}

// ------------------------------------------------------------------ entry point

// UWP, non-XAML: the app model drives our IFrameworkView through CoreApplication::Run.
[Platform::MTAThread]
int __cdecl main(Platform::Array<Platform::String^>^ /*argv*/)
{
    auto source = ref new AppSource();
    CoreApplication::Run(source);
    return 0;
}
