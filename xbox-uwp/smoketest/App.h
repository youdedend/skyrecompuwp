#pragma once

// Xbox UWP smoke test: a D3D11 CoreWindow app.
//
// What it proves, in one deploy:
//   * a package signed with a self-signed cert registers on the console
//   * UWP activation works (CoreApplication::Run + IFrameworkView)
//   * a D3D11 device and a CoreWindow swap chain can present on the console
//   * the app can exit cleanly and return to the Developer Mode shell
//
// It is intentionally tiny. It is NOT the ported runtime; see SkyRecompUwp/ for the
// gated project that the real renderer would go into.

#include <windows.foundation.h>
#include <windows.applicationmodel.activation.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.graphics.display.h>
#include <windows.system.h>

namespace SkyRecompUwpSmoke {

    // Seconds of animation before the app exits by itself, so a console session can
    // never end up stuck in a black window that needs a hard power cycle.
    constexpr double kAutoExitSeconds = 15.0;

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

        // state
        Microsoft::WRL::ComPtr<ID3D11Device>            m_d3dDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>     m_d3dContext;
        Microsoft::WRL::ComPtr<IDXGISwapChain1>         m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  m_renderTargetView;

        Windows::Foundation::Size m_windowBounds;
        bool m_windowClosed;
        bool m_visible;
        LARGE_INTEGER m_qpcFrequency;
        LARGE_INTEGER m_qpcStart;
    };

    // The factory that hands our view to the UWP application model.
    ref class AppSource sealed : public Windows::ApplicationModel::Core::IFrameworkViewSource
    {
    public:
        virtual Windows::ApplicationModel::Core::IFrameworkView^ CreateView();
    };
}
