#pragma once

// Xbox UWP smoke test: a D3D11 CoreWindow app.
//
// What it proves, in one deploy:
//   * a package signed with a self-signed cert registers on the console
//   * UWP activation works (CoreApplication::Run + IFrameworkView)
//   * a D3D11 device and a CoreWindow swap chain can present on the console
//   * the app can exit cleanly and return to the Developer Mode shell
//
// v0.2.0 hardening, after the console crash report:
//   * every stage is appended to  <LocalState>\smoke.log  (LocalState was empty before
//     because v0.1.x only used OutputDebugString, which is invisible on a console)
//   * every framework entry point is wrapped in try/catch, so a failure logs itself
//     instead of terminating the process with no evidence
//   * the screen colour encodes the furthest stage reached, so even a screenshot
//     without the log narrows the failure down
//
// It is intentionally tiny. It is NOT the ported runtime; see SkyRecompUwp/ for the
// gated project that the real renderer would go into.

#include <windows.foundation.h>
#include <windows.applicationmodel.activation.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.graphics.display.h>
#include <windows.storage.h>
#include <windows.system.h>

namespace SkyRecompUwpSmoke {

    // Seconds of animation before the app exits by itself, so a console session can never
    // end up stuck in a window that needs a hard power cycle.
    constexpr double kAutoExitSeconds = 15.0;


    // Furthest stage reached. Also selects the clear colour.
    enum class Stage
    {
        Start            = 0,   // grey  - process started, logging initialised
        AppInitialized   = 1,   // blue  - Initialize() returned
        WindowSet        = 2,   // teal  - SetWindow() returned
        DeviceCreated    = 3,   // blue  - D3D11 device created
        SwapChainCreated = 4,   // teal  - swap chain + RTV ready
        Running          = 5,   // animated - present loop running
        Failed           = 6,   // red   - a stage threw; log has the HRESULT
    };

    ref class App sealed : public Windows::ApplicationModel::Core::IFrameworkView
    {
    public:
        App();

        // IFrameworkView
        virtual void Initialize(Windows::ApplicationModel::Core::CoreApplicationView^ applicationView);
        virtual void SetWindow(Windows::UI::Core::CoreWindow^ window);
        virtual void Load(Platform::String^ entryPoint);
        virtual void Run();
        virtual void Uninitialize();

    private:
        // lifecycle
        void OnActivated(Windows::ApplicationModel::Core::CoreApplicationView^ sender, Windows::ApplicationModel::Activation::IActivatedEventArgs^ args);
        void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ args);
        void OnResuming(Platform::Object^ sender, Platform::Object^ args);
        void OnWindowClosed(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::CoreWindowEventArgs^ args);
        void OnWindowSizeChanged(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::WindowSizeChangedEventArgs^ args);
        void OnKeyDown(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ args);

        // d3d
        void CreateDeviceResources();
        void CreateWindowSizeDependentResources();
        void ReleaseWindowSizeDependentResources();
        void Render(double elapsedSeconds);

        // diagnostics
        void SetStage(Stage stage);
        void Fail(const char* what, HRESULT hr);

        // state
        Microsoft::WRL::ComPtr<ID3D11Device>            m_d3dDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>     m_d3dContext;
        Microsoft::WRL::ComPtr<IDXGISwapChain1>         m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  m_renderTargetView;

        // Windows::UI::Core::CoreWindow::Bounds is a Windows::Foundation::Rect.
        // Declaring this as a Size fails with C2679 on assignment.
        Windows::Foundation::Rect m_windowBounds;
        bool m_windowClosed;
        bool m_visible;
        LARGE_INTEGER m_qpcFrequency;
        LARGE_INTEGER m_qpcStart;

        Stage m_stage;
        double m_failedAt;          // seconds since start when Fail() ran
        bool m_logReady;
    };

    // The factory that hands our view to the UWP application model.
    ref class AppSource sealed : public Windows::ApplicationModel::Core::IFrameworkViewSource
    {
    public:
        virtual Windows::ApplicationModel::Core::IFrameworkView^ CreateView();
    };
}
