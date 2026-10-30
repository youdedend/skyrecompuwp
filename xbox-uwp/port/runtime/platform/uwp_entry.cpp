// Activation host for SkyRecomp on Xbox (UWP, Developer Mode).
//
// A UWP app is started by naming an activatable class in the manifest's EntryPoint, so this file
// supplies one: SkyRecomp.Uwp.App. It does three things and nothing else.
//
//   1. Resolves the two directories UWP gives an app - LocalState and the install location - with
//      WinRT, which is the only exact source, and hands them to the C platform layer before
//      anything opens a file. The platform layer's own fallbacks are guesses; these are not.
//   2. Refuses to start on a memory budget that cannot run the game, with the fix in the message.
//      A Dev Mode app whose Type is not "Game" gets a fraction of the 5,120 MiB budget, and the
//      symptom without this check is an out-of-memory deep inside the renderer.
//   3. Runs the runtime on this thread - the view's Run() thread, which is the app's UI thread and
//      the one SDL's WinRT backend will expect to own the CoreWindow.
//
// Written in C++/CX because that is what produces the .winmd entry point the manifest names.
// C++/WinRT cannot: it has no activatable class for the app model to find. That means this one
// file is compiled by MSVC (/ZW), while the runtime next to it is compiled by clang-cl - see
// port/BUILD-PORT.md, and note that both are MSVC x64 ABI, which is what makes the mix legal.
//
// It has never been run: no console, and the graphics/audio dependencies are not built for UWP.

#include <windows.h>

#include <windows.applicationmodel.core.h>
#include <windows.applicationmodel.h>
#include <windows.foundation.h>
#include <windows.graphics.display.h>
#include <windows.storage.h>
#include <windows.system.h>
#include <windows.system.profile.h>
#include <windows.ui.core.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ---- the C side (port/runtime/platform/uwp.c), and the runtime's entry ---------------------
// main.c is compiled with -Dmain=skyrecomp_main under SKYRECOMP_UWP: this host owns the real
// entry point, and two symbols called main cannot coexist. See CMakeLists.txt.
extern "C" void uwp_set_local_state(const wchar_t *path);
extern "C" void uwp_set_install_dir(const wchar_t *path);
extern "C" bool uwp_present_set_core_window(void *core_window_abi, uint32_t width, uint32_t height);
extern "C" int skyrecomp_main(int argc, char **argv);

namespace SkyRecomp {
namespace Uwp {

namespace {

// A failure before the runtime's own log exists has to land somewhere readable: on a console
// there is no console, and OutputDebugString needs a debugger attached. LocalState is what the
// Device Portal can show.
void entry_log(const wchar_t *line) {
    try {
        auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
        std::wstring path(local->Data());
        path += L"\\entry.log";
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fwprintf(f, L"[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, line);
            fclose(f);
        }
    } catch (...) {
        // Nothing left to do: the message is already gone with the exception.
    }
}

// A crash on a console leaves nothing behind: no console, no debugger, no crash dump the Device
// Portal will show you. The first version of this package did exactly that - it started, died, and
// LocalState was empty, which told us nothing. So every way this process can end writes one line
// first, and the runtime's own log is flushed so the last thing it said survives too.
void log_hresult(const wchar_t *stage, HRESULT hr) {
    wchar_t line[256];
    // The stage text is a literal in this file and always short; the cast silences the 260 that
    // the format would otherwise demand without weakening the copy.
    swprintf_s(line, L"%s failed: 0x%08X", stage, static_cast<unsigned>(hr));
    entry_log(line);
}

void flush_logs() {
    // The runtime's log is a FILE* in uwp.c and this file's is opened per line, so the CRT's own
    // flush point covers both.
    ::_flushall();
}

// Lifecycle and failure events are registered as early as possible - in Initialize(), before the
// window exists - because the failures worth catching are the ones that happen before there is
// anywhere to catch them. Handlers are free of captures so they survive whatever the app model
// does with the delegates.
void register_lifetime_handlers() {
    using namespace Windows::ApplicationModel::Core;

    // Fires on the UI thread for an exception that reached the top of a message. It does not fire
    // for a fail-fast (access violation, stack overflow) - nothing in user mode does - so this is
    // diagnostic coverage, not a safety net.
    CoreApplication::UnhandledException +=
        ref new UnhandledExceptionEventHandler([](Platform::Object ^, UnhandledExceptionEventArgs ^ e) {
            wchar_t line[512];
            const wchar_t *msg = e->Message ? e->Message->Data() : L"(no message)";
            HRESULT hr = e->Exception ? e->Exception->HResult : E_FAIL;
            swprintf_s(line, L"UNHANDLED %s (0x%08X)", msg, static_cast<unsigned>(hr));
            entry_log(line);
            flush_logs();
            e->Handled = false;  // let the app model do what it would have done anyway
        });

    // The async half: an IAsyncAction or IAsyncOperation completed with an error that nobody
    // observed. This is where an out-of-memory in the renderer's setup would surface.
    CoreApplication::UnhandledErrorDetected +=
        ref new UnhandledErrorDetectedEventHandler([](Platform::Object ^, UnhandledErrorDetectedEventArgs ^ e) {
            try {
                const wchar_t *msg = e->UnhandledError->Message ? e->UnhandledError->Message->Data() : L"(no message)";
                std::wstring line(L"UNHANDLED ASYNC ");
                line += msg;
                entry_log(line.c_str());
                flush_logs();
            } catch (...) {
            }
        });

    // On Xbox the app is suspended when the user returns to the dashboard, and there is a short
    // budget to finish up. Nothing here needs to save yet (the runtime's saves are its own), but
    // the log must be on disk, or the session that ended badly leaves no trace at all.
    CoreApplication::Suspending +=
        ref new Windows::Foundation::EventHandler<Windows::ApplicationModel::SuspendingEventArgs ^>(
            [](Platform::Object ^, Windows::ApplicationModel::SuspendingEventArgs ^ e) {
            (void)e;
            entry_log(L"SUSPENDED (returning to the dashboard)");
            flush_logs();
        });

    CoreApplication::Resuming +=
        ref new Windows::Foundation::EventHandler<Platform::Object ^>([](Platform::Object ^, Platform::Object ^) {
            entry_log(L"RESUMED");
        });

    CoreApplication::Exiting +=
        ref new Windows::Foundation::EventHandler<Platform::Object ^>([](Platform::Object ^, Platform::Object ^) {
            entry_log(L"EXITING");
            flush_logs();
        });
}

[[noreturn]] void entry_fail(const wchar_t *stage, const wchar_t *why) {
    std::wstring text(stage);
    text += L": ";
    text += why;
    entry_log(text.c_str());
    ::OutputDebugStringW(text.c_str());
    // Terminating with a non-zero status is all a console lets us do here; the log carries why.
    ::ExitProcess(2);
}

}  // namespace

ref class App sealed : public Windows::ApplicationModel::Core::IFrameworkView {
public:
    // Called before the window exists. Nothing WinRT-storage-related is done here yet: the
    // folder objects are read in Load(), which is documented to run after this.
    virtual void Initialize(Windows::ApplicationModel::Core::CoreApplicationView ^ appView) {
        (void)appView;
        // Before anything else can fail: from here on, every failure writes a line to
        // <LocalState>\entry.log instead of vanishing.
        register_lifetime_handlers();
        entry_log(L"activated");
    }

    virtual void SetWindow(Windows::UI::Core::CoreWindow ^ window) {
        m_window = window;
        // Hand the window to the presentation bridge here rather than in Run(): this is the moment
        // it exists, and DXGI must be able to reach it before the renderer creates its swapchain.
        // reinterpret_cast to IUnknown* is how a C++/CX handle becomes its ABI pointer.
        // Bounds are in effective pixels; the swapchain needs physical ones, so scale by DPI.
        auto bounds = window->Bounds;
        uint32_t pw = static_cast<uint32_t>(bounds.Width);
        uint32_t ph = static_cast<uint32_t>(bounds.Height);
        try {
            float dpi = Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->LogicalDpi;
            pw = static_cast<uint32_t>(bounds.Width * dpi / 96.0f + 0.5f);
            ph = static_cast<uint32_t>(bounds.Height * dpi / 96.0f + 0.5f);
        } catch (...) {
            // No display information: 100% scaling is the right guess for a console.
        }
        if (!uwp_present_set_core_window(reinterpret_cast<::IUnknown *>(window), pw, ph)) {
            entry_log(L"SetWindow: the presentation bridge refused the CoreWindow");
        }

        // A change of display mode changes Bounds without a new call to SetWindow. The bridge is
        // asked for its size once per frame, so handing it the new one here is all the renderer
        // needs to see: it recreates the swapchain when the request and the current one differ.
        window->SizeChanged +=
            ref new Windows::Foundation::TypedEventHandler<Windows::UI::Core::CoreWindow ^,
                                                          Windows::UI::Core::WindowSizeChangedEventArgs ^>(
                [](Windows::UI::Core::CoreWindow ^ w, Windows::UI::Core::WindowSizeChangedEventArgs ^ e) {
                    (void)e;
                    auto bounds = w->Bounds;
                    float dpi = 96.0f;
                    try {
                        dpi = Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->LogicalDpi;
                    } catch (...) {
                    }
                    uint32_t nw = static_cast<uint32_t>(bounds.Width * dpi / 96.0f + 0.5f);
                    uint32_t nh = static_cast<uint32_t>(bounds.Height * dpi / 96.0f + 0.5f);
                    if (!uwp_present_set_core_window(reinterpret_cast<::IUnknown *>(w), nw, nh)) return;
                    wchar_t line[128];
                    swprintf_s(line, L"window resized: %ux%u", nw, nh);
                    entry_log(line);
                });
    }

    virtual void Load(Platform::String ^ entryPoint) {
        (void)entryPoint;
        try {
            auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
            auto install = Windows::ApplicationModel::Package::Current->InstalledLocation->Path;

            // Platform::String is UTF-16; the C layer's signatures are wchar_t*. They are the same
            // width on Windows, and the cast is the documented way across the two types.
            uwp_set_local_state(reinterpret_cast<const wchar_t *>(local->Data()));
            uwp_set_install_dir(reinterpret_cast<const wchar_t *>(install->Data()));

            std::wstring line(L"paths: local=");
            line += local->Data();
            line += L" install=";
            line += install->Data();
            entry_log(line.c_str());

            // Which console and which OS build: when a package behaves differently on one machine
            // than another, this is the first question and the answer is otherwise unavailable.
            try {
                auto version = Windows::System::Profile::AnalyticsInfo::VersionInfo;
                std::wstring info(L"platform: family=");
                info += version->DeviceFamily->Data();
                info += L" version=";
                info += version->DeviceFamilyVersion->Data();
                auto id = Windows::ApplicationModel::Package::Current->Id;
                wchar_t pkg[96];
                swprintf_s(pkg, L" package=%u.%u.%u.%u", id->Version.Major, id->Version.Minor,
                           id->Version.Build, id->Version.Revision);
                info += pkg;
                entry_log(info.c_str());
            } catch (...) {
                entry_log(L"platform: could not read the OS version");
            }
        } catch (Platform::Exception ^ e) {
            entry_fail(L"resolving the app's directories", e->Message->Data());
        } catch (...) {
            entry_fail(L"resolving the app's directories", L"unknown error");
        }

        // The budget check that turns a wrong Dev Home app type into a clear message instead of an
        // out-of-memory in the middle of the renderer.
        try {
            // A property, not a method: C++/CX exposes it without parentheses.
            unsigned long long limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
            if (limit < (2ull << 30)) {
                entry_log(L"memory limit is under 2 GiB: the app's Type is probably not Game");
                entry_fail(L"memory budget",
                           L"this application is capped below 2 GiB; set its Type to Game in "
                           L"Dev Home on the console and start it again");
            }
            wchar_t msg[128];
            swprintf_s(msg, L"memory budget: %llu MiB", limit / (1024ull * 1024ull));
            entry_log(msg);
        } catch (...) {
            // Not fatal: the check is a courtesy, and failing to read the limit must not stop play.
            entry_log(L"could not read the app memory limit");
        }
    }

    virtual void Run() {
        if (m_window) m_window->Activate();

        // One argument: the runtime reads the game from its own directories, and a packaged app has
        // no command line to speak of.
        char name[] = "skyrecomp";
        char *argv[] = { name, nullptr };

        int rc = 0;
        try {
            rc = skyrecomp_main(1, argv);
        } catch (Platform::Exception ^ e) {
            entry_fail(L"running the game", e->Message->Data());
        } catch (...) {
            entry_fail(L"running the game", L"unhandled exception");
        }

        wchar_t msg[64];
        swprintf_s(msg, L"runtime returned %d", rc);
        entry_log(msg);

        Windows::ApplicationModel::Core::CoreApplication::Exit();
    }

    virtual void Uninitialize() {}

private:
    // C4451 says a ref class held in a member can be marshaled across apartments. Every
    // IFrameworkView callback for one view runs on that view's UI thread, so SetWindow and Run
    // are the same apartment and there is nothing to marshal. (Platform::Agile would be the
    // general answer; it is not in scope for this TU, and wrapping the window to satisfy a
    // warning about a situation that cannot occur would be worse than stating why.)
#pragma warning(push)
#pragma warning(disable : 4451)
    Windows::UI::Core::CoreWindow ^ m_window = nullptr;
#pragma warning(pop)
};

ref class AppSource sealed : public Windows::ApplicationModel::Core::IFrameworkViewSource {
public:
    virtual Windows::ApplicationModel::Core::IFrameworkView ^ CreateView() {
        return ref new App();
    }
};

}  // namespace Uwp
}  // namespace SkyRecomp

// The class the manifest's EntryPoint names is SkyRecomp.Uwp.App; this is only the CRT entry that
// starts the app model. [Platform::MTAThread] keeps it off an STA, which the renderer needs.
[Platform::MTAThread]
int main(Platform::Array<Platform::String ^> ^ args) {
    (void)args;
    try {
        Windows::ApplicationModel::Core::CoreApplication::Run(ref new SkyRecomp::Uwp::AppSource());
    } catch (Platform::Exception ^ e) {
        SkyRecomp::Uwp::entry_log(L"CoreApplication::Run threw");
        (void)e;
        return 1;
    }
    return 0;
}
