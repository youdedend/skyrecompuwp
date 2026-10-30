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
using namespace Windows::Storage;

using namespace SkyRecompUwpSmoke;

// ------------------------------------------------------------------ diagnostics
//
// The v0.1.x build crashed on console leaving LocalState empty, because it only used
// OutputDebugString (no listener on a console) and never opened a file. Everything below
// exists so the next failure is diagnosable remotely.
//
// The log is opened per line with CreateFile2(FILE_APPEND_DATA) and closed immediately:
// no FlushFileBuffers (not in the UWP API allowlist), no handle lifetime to get wrong, and
// a crash between lines can never lose what was already written.

namespace {

    wchar_t g_logPath[MAX_PATH] = { 0 };
    bool g_logPathValid = false;
    LARGE_INTEGER g_qpcFreq = { 0 };
    LARGE_INTEGER g_qpcStart = { 0 };

    double NowSeconds()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_qpcFreq.QuadPart == 0) { return 0.0; }
        return double(now.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart);
    }

    void LogInit()
    {
        if (!QueryPerformanceFrequency(&g_qpcFreq)) { g_qpcFreq.QuadPart = 0; }
        QueryPerformanceCounter(&g_qpcStart);

        try
        {
            // <LocalState> is ApplicationData.LocalFolder: writable from an AppContainer and
            // readable through the Xbox Device Portal file browser.
            String^ folder = ApplicationData::Current->LocalFolder->Path;
            if (folder != nullptr)
            {
                // Platform::String::Data() is const char16*; on Windows char16 and wchar_t are
                // both 16-bit but distinct types, so this cast is required (the ATG samples do
                // the same). String contents here are pure ASCII path characters.
                const wchar_t* p = reinterpret_cast<const wchar_t*>(folder->Data());
                if (wcslen(p) < (MAX_PATH - 16))
                {
                    wcscpy_s(g_logPath, MAX_PATH, L"\\\\?\\");
                    wcscat_s(g_logPath, MAX_PATH, p);
                    wcscat_s(g_logPath, MAX_PATH, L"\\smoke.log");
                    g_logPathValid = true;
                }
            }
        }
        catch (...) { g_logPathValid = false; }
    }

    void Log(const char* fmt, ...)
    {
        char line[1024];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, args);
        va_end(args);

        char out[1152];
        sprintf_s(out, sizeof out, "[%8.3f] %s\r\n", NowSeconds(), line);
        OutputDebugStringA(out);

        if (!g_logPathValid) { return; }
        HANDLE h = CreateFile2(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(h, out, (DWORD)strlen(out), &written, nullptr);
            CloseHandle(h);   // closing flushes metadata; no FlushFileBuffers needed
        }
    }

    inline void ThrowIfFailed(HRESULT hr)
    {
        if (FAILED(hr)) { throw Exception::CreateException(hr); }
    }
}

// ------------------------------------------------------------------ view + factory

App::App()
    : m_windowClosed(false)
    , m_visible(true)
    , m_stage(Stage::Start)
    , m_failedAt(0.0)
    , m_logReady(false)
{
    // Logging starts in Initialize()/main(): ApplicationData::Current is not reliably
    // available during construction.
}

IFrameworkView^ AppSource::CreateView()
{
    return ref new App();
}

void App::SetStage(Stage stage)
{
    m_stage = stage;
    const char* names[] = { "Start", "AppInitialized", "WindowSet", "DeviceCreated",
                            "SwapChainCreated", "Running", "Failed" };
    int idx = (int)stage;
    if (idx < 0 || idx > 6) { idx = 6; }
    Log("stage -> %s", names[idx]);
}

void App::Fail(const char* what, HRESULT hr)
{
    m_failedAt = NowSeconds();
    Log("FAILED: %s (hr=0x%08X)", what, (unsigned)hr);
    SetStage(Stage::Failed);
}

// ------------------------------------------------------------------ framework entry points
// Every one of these is called by the UWP application model outside any try/catch of ours,
// so an escaping exception would terminate the process with no evidence. Each is contained.

void App::Initialize(CoreApplicationView^ applicationView)
{
    try
    {
        if (!m_logReady)
        {
            LogInit();
            m_logReady = true;
            Log("=== SkyRecomp UWP smoke %s ===", "0.2.0");
            Log("log: %ls", g_logPathValid ? g_logPath : L"(no path - logging disabled)");
        }

        applicationView->Activated +=
            ref new TypedEventHandler<CoreApplicationView^, IActivatedEventArgs^>(this, &App::OnActivated);

        CoreApplication::Suspending +=
            ref new EventHandler<SuspendingEventArgs^>(this, &App::OnSuspending);

        CoreApplication::Resuming +=
            ref new EventHandler<Object^>(this, &App::OnResuming);

        SetStage(Stage::AppInitialized);
    }
    catch (Exception^ e)
    {
        Fail("Initialize", e->HResult);
    }
    catch (...)
    {
        Fail("Initialize (non-WinRT exception)", E_FAIL);
    }
}

void App::SetWindow(CoreWindow^ window)
{
    try
    {
        window->SizeChanged +=
            ref new TypedEventHandler<CoreWindow^, WindowSizeChangedEventArgs^>(this, &App::OnWindowSizeChanged);

        window->Closed +=
            ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &App::OnWindowClosed);

        window->KeyDown +=
            ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &App::OnKeyDown);

        m_windowBounds = window->Bounds;
        Log("SetWindow: bounds=%.0fx%.0f", m_windowBounds.Width, m_windowBounds.Height);

        CreateDeviceResources();
        SetStage(Stage::DeviceCreated);

        CreateWindowSizeDependentResources();
        SetStage(Stage::SwapChainCreated);

        SetStage(Stage::WindowSet);
    }
    catch (Exception^ e)
    {
        Fail("SetWindow", e->HResult);
    }
    catch (...)
    {
        Fail("SetWindow (non-WinRT exception)", E_FAIL);
    }
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
    try
    {
        CoreWindow^ window = CoreWindow::GetForCurrentThread();
        if (!window)
        {
            Fail("Run: CoreWindow::GetForCurrentThread returned null", E_UNEXPECTED);
            return;
        }

        Log("Run: entering present loop");
        if (m_stage != Stage::Failed) { SetStage(Stage::Running); }

        while (!m_windowClosed)
        {
            if (m_visible)
            {
                window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

                const double elapsed = NowSeconds();

                if (m_stage != Stage::Failed)
                {
                    if (m_swapChain && m_renderTargetView)
                    {
                        Render(elapsed);
                        HRESULT hr = m_swapChain->Present(1, 0);
                        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
                        {
                            Fail("Present: device removed/reset", hr);
                        }
                        else if (FAILED(hr))
                        {
                            Fail("Present", hr);
                        }
                    }
                    else
                    {
                        // Nothing to present with: keep the error visible instead of dying.
                        if (m_d3dContext && m_renderTargetView)
                        {
                            const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
                            m_d3dContext->ClearRenderTargetView(m_renderTargetView.Get(), red);
                        }
                    }

                    if (elapsed >= kAutoExitSeconds)
                    {
                        Log("auto exit after %.1fs (stage reached)", elapsed);
                        CoreApplication::Exit();
                        break;
                    }
                }
                else
                {
                    // Failed: show red for a while so the cause is visible in a screenshot,
                    // then leave rather than sitting on a dead window forever.
                    if (m_renderTargetView && m_d3dContext)
                    {
                        const float red[4] = { 0.8f, 0.05f, 0.05f, 1.0f };
                        m_d3dContext->ClearRenderTargetView(m_renderTargetView.Get(), red);
                        if (m_swapChain) { m_swapChain->Present(1, 0); }
                    }
                    if (NowSeconds() - m_failedAt > 30.0)
                    {
                        Log("failed window elapsed, exiting");
                        CoreApplication::Exit();
                        break;
                    }
                }
            }
            else
            {
                window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            }
        }

        Log("Run: loop exited");
    }
    catch (Exception^ e)
    {
        Fail("Run", e->HResult);
    }
    catch (...)
    {
        Fail("Run (non-WinRT exception)", E_FAIL);
    }
}

// ------------------------------------------------------------------ events

void App::OnActivated(CoreApplicationView^ /*sender*/, IActivatedEventArgs^ /*args*/)
{
    try
    {
        CoreWindow^ window = CoreWindow::GetForCurrentThread();
        if (window) { window->Activate(); }
        Log("activated");
    }
    catch (Exception^ e)
    {
        Fail("OnActivated", e->HResult);
    }
    catch (...)
    {
        Fail("OnActivated (non-WinRT exception)", E_FAIL);
    }
}

void App::OnSuspending(Object^ /*sender*/, SuspendingEventArgs^ /*args*/)
{
    m_visible = false;
    Log("suspending");
}

void App::OnResuming(Object^ /*sender*/, Object^ /*args*/)
{
    m_visible = true;
    Log("resuming");
}

void App::OnWindowClosed(CoreWindow^ /*sender*/, CoreWindowEventArgs^ /*args*/)
{
    m_windowClosed = true;
    Log("window closed");
}

void App::OnWindowSizeChanged(CoreWindow^ sender, WindowSizeChangedEventArgs^ /*args*/)
{
    try
    {
        m_windowBounds = sender->Bounds;
        Log("size changed: %.0fx%.0f", m_windowBounds.Width, m_windowBounds.Height);
        if (m_d3dDevice) { CreateWindowSizeDependentResources(); }
    }
    catch (Exception^ e)
    {
        Fail("OnWindowSizeChanged", e->HResult);
    }
    catch (...)
    {
        Fail("OnWindowSizeChanged (non-WinRT exception)", E_FAIL);
    }
}

void App::OnKeyDown(CoreWindow^ /*sender*/, KeyEventArgs^ args)
{
    if (args->VirtualKey == VirtualKey::Escape || args->VirtualKey == VirtualKey::GoBack)
    {
        Log("exit requested by key 0x%02X", (unsigned)args->VirtualKey);
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
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG,
            levels, _countof(levels), D3D11_SDK_VERSION,
            m_d3dDevice.GetAddressOf(), &got, m_d3dContext.GetAddressOf());
    }
#endif

    if (FAILED(hr))
    {
        Fail("D3D11CreateDevice", hr);
        return;   // keep the process alive; Run() will show red and log the reason
    }

    Log("D3D11CreateDevice ok, feature level 0x%X", (unsigned)got);
}

void App::CreateWindowSizeDependentResources()
{
    if (!m_d3dDevice) { return; }

    ReleaseWindowSizeDependentResources();

    CoreWindow^ window = CoreWindow::GetForCurrentThread();
    if (!window) { Fail("CreateWindowSizeDependentResources: no CoreWindow", E_UNEXPECTED); return; }

    const float dpi = DisplayInformation::GetForCurrentView()->LogicalDpi;
    const float scale = dpi / 96.0f;

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

        Log("swap chain created %ux%u (flip sequential)", width, height);
    }
    else
    {
        ThrowIfFailed(m_swapChain->ResizeBuffers(
            2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        Log("swap chain resized %ux%u", width, height);
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())));
    ThrowIfFailed(m_d3dDevice->CreateRenderTargetView(
        backBuffer.Get(), nullptr, m_renderTargetView.GetAddressOf()));

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

    // Stage-coloured, so a screenshot alone narrows the failure down.
    float clear[4];
    switch (m_stage)
    {
    case Stage::Running:
    {
        // Animated: a frozen frame is immediately distinguishable from a working one.
        const float t = float(elapsedSeconds);
        const float r = 0.5f + 0.5f * sinf(t * 1.7f);
        const float g = 0.5f + 0.5f * sinf(t * 2.3f + 2.1f);
        const float b = 0.5f + 0.5f * sinf(t * 1.1f + 4.2f);
        clear[0] = r * 0.35f; clear[1] = g * 0.35f; clear[2] = b * 0.45f; clear[3] = 1.0f;
        break;
    }
    case Stage::Failed:  clear[0] = 0.80f; clear[1] = 0.05f; clear[2] = 0.05f; break;  // red
    case Stage::Start:   clear[0] = 0.10f; clear[1] = 0.10f; clear[2] = 0.10f; break;  // grey
    case Stage::AppInitialized:   clear[0] = 0.05f; clear[1] = 0.15f; clear[2] = 0.80f; break; // blue
    case Stage::WindowSet:        clear[0] = 0.05f; clear[1] = 0.65f; clear[2] = 0.65f; break; // teal
    case Stage::DeviceCreated:    clear[0] = 0.20f; clear[1] = 0.20f; clear[2] = 0.90f; break; // indigo
    case Stage::SwapChainCreated: clear[0] = 0.10f; clear[1] = 0.80f; clear[2] = 0.30f; break; // green
    default: clear[0] = clear[1] = clear[2] = 0.0f; break;
    }
    if (m_stage != Stage::Failed) { clear[3] = 1.0f; }

    m_d3dContext->ClearRenderTargetView(m_renderTargetView.Get(), clear);
}

// ------------------------------------------------------------------ entry point

// UWP, non-XAML: the app model drives our IFrameworkView through CoreApplication::Run.
[Platform::MTAThread]
int __cdecl main(Platform::Array<Platform::String^>^ /*argv*/)
{
    // Log before anything else can fail, so even an early death leaves a file behind.
    LogInit();
    Log("=== SkyRecomp UWP smoke 0.2.0 main() ===");
    Log("log: %ls", g_logPathValid ? g_logPath : L"(no path - logging disabled)");

    auto source = ref new AppSource();
    Log("AppSource created, calling CoreApplication::Run");
    CoreApplication::Run(source);
    Log("CoreApplication::Run returned");
    return 0;
}
