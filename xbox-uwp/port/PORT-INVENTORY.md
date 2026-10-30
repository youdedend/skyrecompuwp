# Port inventory — every change the Xbox UWP port needs

Measured against `Apolinario-coder/SkyRecomp` at **`f005a61`** (`v2.0.2`), not estimated.
Commands and their output are in [`../../audit/commands.log`](../../audit/commands.log) where
they were run; the rest are reproduced inline below so each number can be re-derived.

The runtime and tools are **149 source files / 39,322 lines**. The port is not a rewrite of that:
it is one replaced file, four small patches, and two design changes that no amount of patching
removes.

| # | What | Kind | Status |
|---|------|------|--------|
| 1 | `runtime/platform/uwp.c` — 25 `plat_*` functions | new file, replaces `windows.c` | written, **never compiled** |
| 2 | `gamelib.c` — `LoadPackagedLibrary` | 18-line patch | written, applies cleanly |
| 3 | `gamebuild.c` — no on-console compiler | 7-line patch | written, applies cleanly |
| 4 | `mods.c` — no shell folder-open | 6-line patch | written, applies cleanly |
| 5 | `main.c` — no re-exec on collision | 10-line patch | written, applies cleanly |
| 6 | Game code must live **inside the package** | design change | analysed, not built |
| 7 | User files reachable only through brokering | design change | analysed, not built |
| 8 | Mesa Dozen + `dxil.dll` | dependency to build | analysed, not built |
| 9 | SDL3 + SDL2's WinRT backend ported in | dependency to build | analysed, not built |
| 10 | C++/CX activation host, CMake project | new files | CMake written; host **not written** |

---

## 1. The platform contract

`runtime/platform.h` declares exactly 25 functions. Each OS supplies them in one file
(`windows.c`, `macos.c`, `linux.c`); the runtime calls nothing else platform-specific from the
host. `uwp.c` implements all 25:

```
plat_init  plat_now_ns  plat_sleep_until_ns  plat_thread_realtime
plat_waiter_create  plat_waiter_destroy  plat_waiter_reset  plat_waiter_signal  plat_waiter_wait
plat_reserve_guest  plat_commit  plat_protect  plat_reset_guest  plat_written_pages
plat_exe_path  plat_exec  plat_spawn  plat_proc_wait  plat_proc_kill  plat_remove_tree
plat_terminate  plat_fatal_notice  plat_symbol  plat_print_backtrace  plat_crash_handler_install
```

Four of them cannot be implemented honestly and are deliberately degraded:

| Function | Desktop behaviour | UWP | Consequence |
|---|---|---|---|
| `plat_spawn` | `CreateProcessW` | logs and returns 0 | callers take their documented failure path |
| `plat_exec` | re-exec self | logs, does nothing | no self-restart; see patch 5 |
| `plat_fatal_notice` | `MessageBoxA` | writes `OutputDebugStringA` | reason reaches the log, not the player's face |
| `plat_symbol` / backtrace | `GetModuleHandleExA` + dbghelp | stub | guest function names from `dispatch.c` still appear in the log |

Everything else has a real implementation: `VirtualAlloc`/`VirtualProtect` for the guest window,
`GetWriteWatch` with a documented fallback, `CreateWaitableTimerExW` for frame pacing,
`QueryPerformanceCounter` for `plat_now_ns`.

**The logging gap was the important find.** `windows.c` redirects `stdout`/`stderr` to
`skyrecomp.log` for GUI-subsystem builds (`log_to_file_without_console`, line 197). My first
`uwp.c` did not, which would have reproduced exactly the failure the user reported on v0.2.0 —
an installed app that dies without leaving anything in `LocalState`. `uwp.c` now mirrors that
function with the destination moved to `LocalState` (the package directory is read-only), keeps
the previous run as `skyrecomp.prev.log`, sets `_IONBF`, and carries the same `require_cpu()`
guard (which must not use a message box).

---

## 2. AppContainer violations — before and after

`port/check-uwp-allowlist.py` evaluates `#if`/`#elif` with the UWP macro set, so desktop-only
branches are not counted. Run against the pristine tree:

```
$ python3 port/check-uwp-allowlist.py <upstream>/runtime
  hard violations: 14    soft concerns: 3
```

All 14 are in three files: `platform/windows.c` (the file being replaced), `gamelib.c:60`
(`LoadLibraryA`), `mods.c:76` (`ShellExecuteA`, plus two `system()` calls on Apple/Linux paths).

After `uwp.c` replaces `windows.c` and the four patches apply:

```
$ python3 port/check-uwp-allowlist.py <upstream>/runtime --fail \
      --exclude 'platform/windows.c' --exclude 'platform/linux.c' --exclude 'platform/macos.c'
  hard violations: 0    soft concerns: 1
  [soft] gamelib.c:70  GetProcAddress
```

`GetProcAddress` is legitimate there: it resolves `lasgame_tables` out of the DLL that
`LoadPackagedLibrary` just loaded. The check is usable as a CI gate in that exact form, and it
costs milliseconds, so a regression (someone adding `CreateProcessW` to a new file) fails the
build instead of the console.

The three `platform/*.c` files are excluded because the UWP build does not compile them at all —
`CMakeLists.txt` filters them out of the source list explicitly.

---

## 3. The four patches

`port/patches/`, generated with `git diff -U6` against `f005a61`, each verified with
`git apply --check` against a pristine tree, and all four verified to apply together.

**`0001-gamelib-load-packaged-library.patch`.** The game code is a separate DLL the runtime
loads at startup (`runtime/gamelib.c`, `skygame.dll`, export `lasgame_tables`). An AppContainer
loads only DLLs that are part of the package, only by a package-relative path, and refuses
absolute paths and `..`. So `LoadLibraryA(path)` becomes `LoadPackagedLibrary(L"skygame.dll", 0)`
and the "hasn't been built" message becomes actionable ("this package has no game code in it").
This is the patch that makes the port's whole distribution model explicit — see section 4.

**`0002-gamebuild-no-on-console-compiler.patch`.** `gamebuild_available()` returns false under
UWP. This is the single point that turns off the in-app build path (`tools/gen.py` + bundled
`zig cc` → `skygame.dll`) without touching the 600 lines that implement it. The start screen
then shows its existing "not available" state instead of trying to spawn a compiler that cannot
exist.

**`0003-mods-no-shell-open.patch`.** `mods_open_folder()` creates the directory and logs;
no `ShellExecuteA`. There is no Explorer on a console and the mods folder is in `LocalState`.

**`0004-main-no-reexec.patch`.** `main.c`'s constructor reserves the 12.5 GB guest window at
`0x7000000000`; on collision the desktop build re-execs itself for a fresh layout. A UWP process
cannot restart itself. The patch replaces the retry with a message naming the cause
(`/HIGHENTROPYVA:NO`) instead of pretending to recover.

---

## 4. Design change: the game code is inside the package

This is the finding that reshapes the port, and it is not a bug in the upstream design — it is
what "ship no game content" means once you cannot load code from disk.

Upstream, on a PC: the player points the start screen at their game files, the app runs
`tools/gen.py` and the bundled `zig cc`, writes `<game_dir>/skygame.dll`, and loads it. Three
of those four steps are impossible in an AppContainer: no child processes, no compiler on the
console, no loading code from a user directory.

The port keeps the model but moves the build to the PC that builds the package:

```
PC (Windows, with the toolchain)                    Xbox (Developer Mode)
  your own game dump                                installed package, read-only
        |                                            ├── skyrecomp_uwp.exe
        v                                            ├── skygame.dll        <- LoadPackagedLibrary
  make gen && make            (unchanged, upstream)  ├── vulkan_dzn.dll     <- Mesa Dozen
        |                                            ├── dxil.dll
        v                                            └── SDL3.dll
  skyrecomp_uwp.exe + skygame.dll + deps
        |
        v
  Package-Uwp.ps1  ->  skyrecomp-uwp.msix  ->  sideload
```

Consequences, stated plainly:

* **There is no downloadable universal package.** A package contains the player's game code, so
  each player builds their own from their own files. This is the same posture as every other
  recompilation port, and it is why the repo ships no game content.
* **The `GAMELIB_ID` mechanism still does its job.** A `skygame.dll` built for a different
  runtime version is refused by `id_matches()` before it is loaded, with the existing message.
* **The .id file has to travel with the DLL.** `id_matches()` reads `<lib>.id` next to the DLL;
  the packaging script must include `skygame.dll.id`, or every load reports "built for another
  version".
* **Writable state goes to `LocalState`**, not next to the executable: shader caches, settings,
  mods, saved screenshots. The runtime already has `paths.h` for this; the port's job is to make
  its root resolve to `LocalState` instead of the exe directory.
* **A static-linked alternative exists** (link the generated game code straight into the exe and
  return a fixed `GameLibTables`), which would remove the DLL and the id check. It needs changes
  in `gamelib.c` and a much larger link step, and it buys nothing for now. Not doing it.

---

## 5. Design change: file I/O has to be brokered

Upstream uses POSIX file APIs throughout, which map onto UWP fine **as long as the paths are
legal**. Census over `runtime/` + `tools/`:

| call | count | call | count |
|---|---|---|---|
| `fopen` | 96 | `fread` | 35 |
| `fwrite` | 38 | `mkdir` | 32 |
| `stat` | 28 | `pread` | 14 |
| `readdir` | 13 | `opendir` | 12 |
| `open` | 7 | `rename` | 6 |
| `unlink` | 4 | | |

The calls are portable; the **roots** are not. Legal, without a picker: the package install
directory (read-only), `LocalState`, and `D:\DevelopmentFiles` on the console's own drive. Anything
else — an arbitrary folder, a USB stick the user plugged in — needs `FileOpenPicker`/`FolderPicker`
from the UI thread, which returns a token that must be persisted and re-opened on the next launch.

That brokering lives in the UI layer, not in `uwp.c`: `plat_*` has no directory-picking function,
and adding one would be a change to `platform.h`, which the port is trying to avoid. The practical
answer for a first playable is: game files and mods live under `LocalState` (copied in over the
Device Portal or extracted from USB by an existing UI path), no picker, no token bookkeeping.

---

## 6. Memory

| Fact | Value | Source |
|---|---|---|
| guest window | `0x7000000000`–`0x7320000000`, 12.5 GB | `runtime/rt.h` `GUEST_LO`/`GUEST_HI` |
| reservation | `MEM_RESERVE` in `constructor(101)`, before the runtime starts | `runtime/main.c` |
| commit | lazy, 64 KiB at a time, by the vectored exception handler | `windows.c` / `uwp.c` |
| reset (in-place reboot) | `MEM_DECOMMIT` | `plat_reset_guest` |
| Series X Dev Mode budget | **5,120 MiB** | gow3 dev log |
| failure mode when over budget | naive reservation fails with **error 487** | gow3 dev log |

Reserving 12.5 GB of *address space* is legal and cheap — reservation is not commit. What breaks
is the committed set: if a scene needs more than the budget, the app is killed, and there is no
pagefile to push it into. The mitigation the gow3 port shipped is a pager that evicts guest pages
to disk (4K pages, ~450 MiB to disk at 45 fps at 720p). That is real work, it is not in this
inventory as a patch, and it is only needed once the game runs far enough to be resident-heavy.
`/HIGHENTROPYVA:NO` is mandatory from day one: the window is fixed, so the image must not be
placed randomly.

`GetWriteWatch` feeds the Vulkan vertex/index cache re-copy in `runtime/nvn/vulkan/exec.c`. If the
console refuses `MEM_WRITE_WATCH`, `plat_written_pages()` returns -1 and the runtime's existing
"no tracking: the renderer reads guest memory in place" path takes over — slower, still correct.
A refusal is not a port blocker.

---

## 7. Correction: SDL3 has no WinRT backend

`PORT-BLUEPRINT.md` section 6 originally said SDL3 ships a WinRT video driver and that two small
patches would bring it to UWP. Enumerating the upstream trees says otherwise:

| ref | paths matching `winrt` |
|---|---|
| `release-3.2.0`, `release-3.2.4`, `release-3.4.0` | 0 |
| `release-3.4.18`, `preview-3.1.3`, `preview-3.1.6` | 0 |
| **`release-2.30.11` (SDL2)** | **24 files, ~180 KiB** |

SDL3 dropped the WinRT app model; SDL2 still has it (`src/core/winrt/`, `src/video/winrt/`,
`src/main/winrt/`). WWHD's CMake calls its dependency a *"Pinned SDL3 UWP source"*, and its
patches target SDL2's file layout while converting SDL2 types to SDL3 ones — i.e. it is a branch
that back-ported the SDL2 WinRT backend into SDL3, not a release plus two patches.

Because the runtime touches **219 distinct `SDL_*` symbols**, the options are:

| option | verdict |
|---|---|
| Port SDL2's WinRT backend into SDL3, carried as a patch series | **chosen**; ~180 KiB of source material, 1.5–3 weeks |
| Pin SDL2 and port 219 SDL3 call sites backwards | worse: rewrites working code |
| Hand-write CoreWindow + WGI + audio | worse: reimplements a driver that exists |

The consequence for this inventory is confined to step 4 of the remaining-work list: the SDL3
dependency is a **fork with a backend port**, not a stock build with two `.patch` files.

Also learned from the port that has done this, and worth carrying over: their entry point is
`wWinMain` + `SDL_RunApp` (SDL supplies the activatable class the manifest names), and they
refuse to start when `MemoryManager.AppMemoryUsageLimit() < 2 GiB`, with the message *"Set this
application to Game in Dev Home and restart"* — the app's **Type must be Game** in Dev Home, or
the memory limit is a fraction of the 5,120 MiB budget.

---

## 8. What is verified, and what is not

| Claim | Evidence |
|---|---|
| 25 `plat_*` functions, no more | `grep -oE 'plat_[a-z_0-9]+'` over `uwp.c` → 25 unique; `platform.h` declares the same set |
| the runtime has no other host dependency | the four-`#ifdef`-branch design of `platform/{windows,macos,linux}.c` |
| 14 hard AppContainer violations exist | checker run, quoted above |
| 4 patches apply to `f005a61` | `git apply --check`, all four, then applied together |
| 0 hard violations after the port | checker run, quoted above |
| game code must be in the package | `LoadPackagedLibrary` documentation + `gamelib.c:60` |
| `uwp.c` **compiles** | **no — never compiled.** No MSVC, no Windows SDK, no UWP workload here |
| `uwp.c` **runs on a console** | **no — never run** |
| Dozen/SDL3/FFmpeg link | **no — none of them built for UWP yet** |

Every place `uwp.c` had to guess is marked `UWP-ASSUMPTION`. Grep for it. The first real
compile is part of the work, not a formality: expect a batch of `WINAPI_FAMILY` errors that are
mechanical, and expect at least one API that turns out to be unavailable and needs the WinRT
replacement instead.

---

## 9. Remaining work, in order

1. **Build `uwp.c` with clang-cl on Windows**, fix the compile errors. Needs the UWP workload
   and SDK 10.0.26100. Cheapest possible step; everything downstream is blocked on it.
2. **Write `port/runtime/platform/uwp_entry.cpp`** — the entry host the manifest names. Shape is
   now known (section 7): `wWinMain` + `SDL_RunApp`, with the WinRT app model coming from the
   SDL fork. It must obtain `ApplicationData.Current.LocalFolder.Path` and
   `Package.Current.InstalledLocation.Path` for `uwp.c` (the `%LOCALAPPDATA%` guess in `uwp.c` is
   a marked fallback only), assert `AppMemoryUsageLimit() >= 2 GiB` so a wrong Dev Home app type
   is a clear error rather than an out-of-memory, and start the log before anything else.
3. **Build Dozen** (`vulkan_dzn.dll`) and `dxil.dll` for UWP. Fork of Mesa, `-Dvulkan-drivers=
   microsoft`, D3D12, DXIL 1.7. Largest single task.
4. **Port SDL2's WinRT backend into SDL3** and build it for UWP (section 7). Largest task after
   Dozen, and on the critical path for a first frame.
5. **Build FFmpeg (LGPL) and shaderc for UWP**, static.
6. **Two-stage packaging**: `make gen && make` on the PC, then the CMake project, then
   `Package-Uwp.ps1` with `skygame.dll` + `skygame.dll.id` in the layout.
7. **First frame on the console**, then the memory pager if and when the budget bites.

Steps 1–5 all need a Windows machine with the UWP workload. They can run on GitHub Actions
`windows-latest` (which already has VS 17.14 + both SDKs, proven by the smoke-test pipeline), but
the game dump has to be supplied by the person building — it cannot be in a public CI job, and it
must not be.
