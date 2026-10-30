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
#include <windows.storage.h>
#include <windows.system.h>
#include <windows.ui.core.h>

#include <cstdio>
#include <cstring>
#include <string>

// ---- the C side (port/runtime/platform/uwp.c), and the runtime's entry ---------------------
// main.c is compiled with -Dmain=skyrecomp_main under SKYRECOMP_UWP: this host owns the real
// entry point, and two symbols called main cannot coexist. See CMakeLists.txt.
extern "C" void uwp_set_local_state(const wchar_t *path);
extern "C" void uwp_set_install_dir(const wchar_t *path);
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
    }

    virtual void SetWindow(Windows::UI::Core::CoreWindow ^ window) {
        m_window = window;
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
        } catch (Platform::Exception ^ e) {
            entry_fail(L"resolving the app's directories", e->Message->Data());
        } catch (...) {
            entry_fail(L"resolving the app's directories", L"unknown error");
        }

        // The budget check that turns a wrong Dev Home app type into a clear message instead of an
        // out-of-memory in the middle of the renderer.
        try {
            unsigned long long limit = Windows::System::MemoryManager::AppMemoryUsageLimit();
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
    Windows::UI::Core::CoreWindow ^ m_window = nullptr;
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
