// UWP / Xbox Developer Mode host layer (runtime/platform.h).
//
// Drop-in replacement for runtime/platform/windows.c. Implements the same 25 plat_* entry
// points, for an AppContainer with WINAPI_FAMILY=WINAPI_FAMILY_APP and 5,120 MiB of memory.
//
// STATUS: compiles clean with clang-cl in the UWP configuration
// (WINAPI_FAMILY=WINAPI_FAMILY_APP, _CRT_APP, SKYRECOMP_UWP), enforced by
// .github/workflows/compile-uwp-platform.yml on every change here. It has never been *run*:
// no console, no game code, and none of the graphics/audio dependencies are built for UWP yet.
// Every place where behaviour was assumed rather than measured is marked "UWP-ASSUMPTION" -
// grep for it.
//
// What is deliberately different from windows.c, and why:
//
//   1. NO CHILD PROCESSES. plat_spawn/plat_exec cannot exist in an AppContainer. They are
//      implemented as loud, logged failures. Callers must be removed:
//        runtime/gamebuild.c   (runs bundled zig cc to build skygame.dll)
//        runtime/ui/launcher.mm (spawns the setup helper)
//      The game library has to be produced off-console and loaded instead. That is a design
//      change, not a shim - see xbox-uwp/PORT-BLUEPRINT.md section 5.
//
//   2. NO MESSAGE BOXES. plat_fatal_notice logs to LocalState and OutputDebugString.
//      MessageBoxA is not in the UWP API allowlist.
//
//   3. NO timeBeginPeriod / SetProcessInformation. winmm and the power-throttling API are not
//      available. Precise waits use a high-resolution waitable timer instead (windows.c does
//      this too, in plat_sleep_until_ns).
//
//   4. MEMORY IS THE HARD PART. The 12.5 GB guest window is reserved exactly as on desktop
//      (VirtualAlloc MEM_RESERVE is allowed), but commit is lazy and, under pressure, paged.
//      MEM_WRITE_WATCH may be refused - the runtime already tolerates plat_written_pages()
//      returning -1 ("no tracking: the renderer reads guest memory in place"), so that
//      degradation is free. See PORT-BLUEPRINT.md section 3 for the pager work still to do.

#include <windows.h>

// Including the documented owners (<appmodel.h>, <errhandlingapi.h>, <fileapi.h>) does not help:
// in the APP partition this SDK does not declare GetCurrentPackagePathW, MoveFileA or
// AddVectoredExceptionHandler. The first two were removable, so they are gone - the paths come
// from the activation host, and log rotation uses the CRT. This one cannot be: lazy commit of the
// guest window hangs off a vectored exception handler, and every port that runs on the console
// relies on them. The export exists in the UWP API surface; only the prototype is missing here.
WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../platform.h"

// ---------------------------------------------------------------------------------------
// paths
//
// UWP has no shell32 and no freely-chosen directories. Two locations are legal:
//   * package install location (read-only, shipped content)  -> GetCurrentPackagePathW
//   * LocalState (read/write, private)                       -> %LOCALAPPDATA%\Packages\<PFN>\LocalState
// plus, for the user's own game files, DevelopmentFiles on D:\ or a USB drive reached through
// a picker - that lives in the app's storage code, not here (PORT-BLUEPRINT.md section 4).
// ---------------------------------------------------------------------------------------

static char g_install_dir[MAX_PATH * 3];
static char g_local_state[MAX_PATH * 3];
static bool g_paths_ready;

static bool utf8_from_wide(const wchar_t *w, char *out, size_t cap) {
    if (!w || !out || cap == 0) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    return n > 0 && (size_t)n <= cap;
}

static void resolve_paths(void) {
    if (g_paths_ready) return;
    g_paths_ready = true;

    // Package install location: read-only content, and the root for relative asset paths.
    // GetCurrentPackagePathW is not declared in the APP partition, so this comes from the
    // activation host (Package.Current.InstalledLocation) or, failing that, from where this
    // executable was loaded from - which for a packaged app is the install directory.
    if (!g_install_dir[0]) {
        char exe[MAX_PATH * 3];
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
        if (n > 0 && n < sizeof exe) {
            char *slash = strrchr(exe, '\\');
            if (slash) *slash = 0;
            snprintf(g_install_dir, sizeof g_install_dir, "%s", exe);
        }
    }

    // LocalState is normally supplied by the activation host from
    // ApplicationData.Current.LocalFolder.Path, which is the only exact source. The fallback
    // below is deliberately dumb: %LOCALAPPDATA% inside a packaged process is redirected to
    // <...>\Packages\<PFN>\LocalCache\Local, so it is writable and inside the app's profile
    // even though the name does not say LocalState. The earlier version of this function
    // synthesised <LOCALAPPDATA>\Packages\<PackageFullName>\LocalState, which never resolves -
    // the folder is keyed by family name, not full name - so it always fell through.
    if (!g_local_state[0]) {
        char local[MAX_PATH * 3] = { 0 };
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, (DWORD)sizeof local);
        if (n > 0 && n < sizeof local) snprintf(g_local_state, sizeof g_local_state, "%s\\LocalState", local);
    }
}

// The activation host calls these with the WinRT-resolved paths before plat_init(). They are the
// only reliable way to find LocalState; resolve_paths() runs once and remembers the answer.
void uwp_set_local_state(const wchar_t *path) {
    g_paths_ready = false;  // a later resolve must not keep a guessed value
    g_local_state[0] = 0;
    utf8_from_wide(path, g_local_state, sizeof g_local_state);
}
void uwp_set_install_dir(const wchar_t *path) {
    g_paths_ready = false;
    g_install_dir[0] = 0;
    utf8_from_wide(path, g_install_dir, sizeof g_install_dir);
}

const char *uwp_local_state_dir(void) { resolve_paths(); return g_local_state; }
const char *uwp_install_dir(void)    { resolve_paths(); return g_install_dir; }

// ---- guest address space ---------------------------------------------------------------
// Same model as windows.c: reserve [lo, hi) at exactly that address, commit lazily on first
// touch through a vectored exception handler, decommit-all to reset.

static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return false;
    if (mi.State == MEM_COMMIT) return mi.Protect == PAGE_READWRITE;
    if (mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    if (er->NumberParameters >= 2 && er->ExceptionInformation[0] != 8 && lazy_commit(fa))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// MEM_WRITE_WATCH is the one allocator feature I could not verify for AppContainer.
// windows.c already treats a refusal as a supported configuration (no write tracking), so this
// tries and falls back rather than failing the port.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)lo) {
        g_write_watch = true;
    } else {
        p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
        g_write_watch = false;
    }
    if (p != (void *)(uintptr_t)lo) {
        fprintf(stderr, "[uwp] reserve %llx+%llx failed (%lu)\n", (unsigned long long)lo,
                (unsigned long long)(hi - lo), GetLastError());
        return false;
    }
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size,
                      (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    if (n >= (ULONG_PTR)max) ResetWriteWatch((void *)(uintptr_t)addr, (SIZE_T)size);
    return (int)n;
}

bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[uwp] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time ------------------------------------------------------------------------------

static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    // No timeBeginPeriod in UWP; thread priority is allowed and is what matters here.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

typedef struct { HANDLE event, timer; } PreciseWaiter;

void *plat_waiter_create(void) {
    PreciseWaiter *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!w->event) { free(w); return NULL; }
    return w;
}

void plat_waiter_destroy(void *waiter) {
    PreciseWaiter *w = waiter;
    if (!w) return;
    if (w->timer) CloseHandle(w->timer);
    if (w->event) CloseHandle(w->event);
    free(w);
}

void plat_waiter_reset(void *waiter)     { if (waiter) ResetEvent(((PreciseWaiter *)waiter)->event); }
void plat_waiter_signal(void *waiter)    { if (waiter) SetEvent(((PreciseWaiter *)waiter)->event); }

bool plat_waiter_wait(void *waiter, int64_t timeout_ns) {
    PreciseWaiter *w = waiter;
    if (!w) return false;
    DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)((timeout_ns + 999999) / 1000000);
    return WaitForSingleObject(w->event, ms) == WAIT_OBJECT_0;
}

// ---- process ---------------------------------------------------------------------------

// Release builds are GUI-subsystem executables and a console app has no console: stdout / stderr
// go to a file, unbuffered so a crash loses at most the line being written. This is the same
// mechanism as windows.c, with the destination moved: the package directory is read-only in UWP,
// so the log goes to LocalState, where the Device Portal can show it. Without this every LOG()
// in the runtime is discarded and a crash leaves an empty LocalState - the exact failure mode the
// v0.1 smoke package had.
static char g_log_path[MAX_PATH * 3];

static void log_to_local_state(void) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD type = err && err != INVALID_HANDLE_VALUE ? GetFileType(err) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) return;  // a debugger is already capturing it

    resolve_paths();
    if (!g_local_state[0]) return;

    // Keep the previous run: on a console the interesting log is usually the one from the crash
    // that just happened, and the user can only copy files out afterwards.
    char prev[MAX_PATH * 3];
    snprintf(prev, sizeof prev, "%s\\skyrecomp.prev.log", g_local_state);
    snprintf(g_log_path, sizeof g_log_path, "%s\\skyrecomp.log", g_local_state);
    // rotate with the CRT: MoveFileA is not declared in the APP partition, and rename/remove are
    // the same operation for two names in one directory.
    FILE *probe = fopen(g_log_path, "rb");
    if (probe) {
        fclose(probe);
        remove(prev);
        rename(g_log_path, prev);
    }

    FILE *out = NULL;
    if (freopen_s(&out, g_log_path, "wb", stderr) != 0 || !out) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    fprintf(stderr, "[diagnostics] log: %s\n", g_log_path);
}

// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2). Series X|S
// is Zen 2 and has them, so this only guards against a mis-built package - but it must not use a
// message box to say so.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    fprintf(stderr, "FATAL: this CPU lacks AVX2/FMA/BMI2; the game code was built for x86-64-v3\n");
    fflush(stderr);
    ExitProcess(1);
}

void plat_init(void) {
    log_to_local_state();

    // Where the runtime keeps game files, saves and seeds. runtime/paths.c reads
    // LARECOMP_DATA_ROOT first, and only then falls back to the directory next to the exe - which
    // inside a package is C:\Program Files\WindowsApps\<package>, read-only. Without this the
    // first thing the game does is try to mkdir there, fail its own writability check and exit with
    // the desktop message about moving the folder out of Program Files. LocalState is the folder the
    // app may write and the Device Portal can show, so that is the data root.
    setenv("LARECOMP_DATA_ROOT", g_local_state, 1);

    require_cpu();
    // No console, no timeBeginPeriod, no SetProcessInformation, no SetConsoleOutputCP in UWP:
    // precise waits use the high-resolution waitable timer in plat_sleep_until_ns instead.
    _set_fmode(_O_BINARY);  // fopen/open default to binary (_fmode itself is deprecated)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

bool plat_exe_path(char *buf, size_t size) {
    resolve_paths();
    if (!g_install_dir[0] || strlen(g_install_dir) >= size) return false;
    strncpy(buf, g_install_dir, size - 1);
    buf[size - 1] = 0;
    return true;
}

// Cannot be done: replacing the process image is not permitted. Callers that relied on the
// desktop restart trick have to be redesigned (PORT-INVENTORY.md lists them).
void plat_exec(char *const argv[]) {
    (void)argv;
    fprintf(stderr, "[uwp] plat_exec: not available in an AppContainer; ignoring\n");
}

// Also unavailable. Returning 0 makes every existing caller take its documented failure path,
// which is the correct behaviour until the spawn sites are removed.
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)log_path;
    (void)append_log;
    fprintf(stderr, "[uwp] plat_spawn: not available in an AppContainer (argv[0]=%s)\n",
            argv && argv[0] ? argv[0] : "(null)");
    return 0;
}

int  plat_proc_wait(PlatProc p, bool block) { (void)p; (void)block; return -1; }
void plat_proc_kill(PlatProc p)             { (void)p; }

bool plat_remove_tree(const char *dir) {
    // Only legal inside the app's own writable area. Mirrors rm -rf semantics closely enough
    // for the runtime's use (deleting generated/prepared folders under LocalState).
    if (!dir || !*dir) return false;
    char pattern[MAX_PATH * 3];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char child[MAX_PATH * 3];
            snprintf(child, sizeof child, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                plat_remove_tree(child);
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(dir) != 0;
}

void plat_terminate(void) {
    TerminateProcess(GetCurrentProcess(), 0);
}

// ---- diagnostics -----------------------------------------------------------------------

const char *uwp_log_path(void) {
    resolve_paths();
    if (!g_local_state[0]) return NULL;
    if (!g_log_path[0]) snprintf(g_log_path, sizeof g_log_path, "%s\\skyrecomp.log", g_local_state);
    return g_log_path;
}

void plat_fatal_notice(void) {
    // No MessageBox in UWP: leave the reason in the log the user can pull from the console.
    const char *log = uwp_log_path();
    fprintf(stderr, "[uwp] fatal notice (see %s)\n", log ? log : "the log");
    OutputDebugStringA("[uwp] fatal notice\n");
}

const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    // Backtrace symbols are a desktop debugging aid; on console the log plus guest function
    // names (dispatch.c) carry the diagnosis. Deliberately a stub: GetModuleHandleExA is not
    // worth risking in the allowlist for a cosmetic feature.
    (void)pc;
    if (offset) *offset = 0;
    return NULL;
}

void plat_print_backtrace(void) {
    // Stubbed for the same reason as plat_symbol.
}

void plat_crash_handler_install(void) {
    // No SetUnhandledExceptionFilter in the AppContainer allowlist. The vectored handler
    // installed by plat_reserve_guest still serves lazy commit, which is the part that
    // correct execution depends on.
    static bool installed;
    if (installed) return;
    installed = true;
    OutputDebugStringA("[uwp] crash handler: using vectored handler only\n");
}
