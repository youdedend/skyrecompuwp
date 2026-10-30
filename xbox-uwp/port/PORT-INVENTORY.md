# Port inventory — every change the Xbox UWP port needs

Measured against `Apolinario-coder/SkyRecomp` at **`f005a61`** (`v2.0.2`), not estimated.
Commands and their output are in [`../../audit/commands.log`](../../audit/commands.log) where
they were run; the rest are reproduced inline below so each number can be re-derived.

The runtime and tools are **149 source files / 39,322 lines**. The port is not a rewrite of that:
it is one replaced file, a five-part patch set, a compatibility header, and two design changes
that no amount of patching removes.

| # | What | Kind | Status |
|---|------|------|--------|
| 1 | `runtime/platform/uwp.c` — 25 `plat_*` functions | new file, replaces `windows.c` | **compiles clean** in CI; never run |
| 2 | `gamelib.c` — `LoadPackagedLibrary` | 18-line patch | written, applies cleanly |
| 3 | `gamebuild.c` — no on-console compiler | 7-line patch | written, applies cleanly |
| 4 | `mods.c` — no shell folder-open | 6-line patch | written, applies cleanly |
| 5 | `main.c` — no re-exec on collision | 10-line patch | written, applies cleanly |
| 6 | `uwp_compat.h` — POSIX names on UCRT | new file + 1-line `platform.h` patch | **compiles clean** in CI |
| 7 | Game code must live **inside the package** | design change | analysed, not built |
| 8 | User files reachable only through brokering | design change | analysed, not built |
| 9 | Mesa Dozen + `dxil.dll` | dependency to build | analysed, not built |
| 10 | SDL3 + SDL2's WinRT backend ported in | dependency to build | analysed, not built |
| 11 | Entry host, CMake project, CI compile gate | new files | host **compiles** in CI; project never configured |
| 12 | `uwp_present.cpp` — D3D12 CoreWindow swapchain | new file | **compiles** in CI; never run |
| 12b | **patch 0006** — `device.c` onto the bridge | 82-line patch, 6 guarded regions | applies cleanly; **not compiled** |
| 13 | `uwp_vk_present.c` — shared Vulkan image, ownership | new file | **compiles** in CI; never run |
| 14 | `uwp_posix/{pthread,dirent,unistd,strings,dlfcn}.h` — POSIX headers MSVC lacks | 5 headers, 372 lines | **compiles** in CI |
| 15 | **patch 0007** — `romfs_build.c` hard links | 23-line patch | **compiles** in CI |

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

## 2b. The first compile, and what it found

`.github/workflows/compile-uwp-platform.yml` builds `uwp.c` with `clang-cl /fsyntax-only` against
the pinned upstream commit in the UWP configuration. Run `41052945145` is green: zero
diagnostics. Getting there took three rounds, and each round was a real defect rather than a CI
detail:

| Round | Reported | Cause |
|---|---|---|
| 1 | nothing — "exit code 1" | GitHub's bash shell injects `-e -o pipefail`, so the failing compiler aborted the step before anything was parsed. |
| 2 | `platform.h:12` selects `win_compat.h` | Git Bash rewrote `/DSKYRECOMP_UWP=1` into a path, so the define never arrived. Dash flags fix it; the probe step now measures this instead of assuming it. |
| 3 | 6 errors, 5 warnings | Real API and CRT problems, listed below. |

What round 3 found:

* `GetCurrentPackagePathW`, `MoveFileA` and `AddVectoredExceptionHandler` are **not declared in
  the APP partition** of this SDK, even with `<appmodel.h>`, `<fileapi.h>` and
  `<errhandlingapi.h>` included explicitly. The first two were removed (the activation host
  supplies the paths; log rotation uses `remove`/`rename`). The third stays and its prototype is
  declared locally, with the reason at the declaration.
* `_fmode` and `freopen` are deprecated by UCRT — `_set_fmode` and `freopen_s` instead.
* The `LocalState` derivation was already known to be wrong; this forced it out. It now has an
  honest fallback plus `uwp_set_local_state()`, which the activation host calls with
  `ApplicationData.Current.LocalFolder.Path`.

---

## 3. The five patches

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

**`0005-platform-uwp-compat-header.patch`.** One `#if` in `platform.h` so a UWP build selects
`platform/uwp_compat.h` instead of the mingw-oriented `win_compat.h`. Mirrors the branch that is
already there for Linux.

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

## 4a. The ABI decides the toolchain — and it changes the PC-side build

`grep -rn "sysv_abi\|ms_abi"` over the whole upstream tree returns **nothing**, and
`runtime/cpu.h` declares `typedef void (*GuestFn)(Cpu *);` — a plain function pointer. So the
calling convention of the recompiled game code is not annotated anywhere: it is simply whatever
compiler produced it.

Upstream, on Windows, that compiler is `zig cc` (mingw target, System V), and it builds *both*
the runtime and the game code, so the two agree.

This port's runtime is clang-cl, i.e. the **MSVC x64 ABI**. If the game code stayed on `zig`,
every call through `GameLibTables` would cross an ABI boundary — different argument registers,
different shadow space, silent corruption. The failure would not look like an ABI problem; it
would look like random crashes in recompiled functions.

Two consequences, both load-bearing:

* **The game code must be built with clang-cl**, not zig, for the UWP package. `-mavx2 -mfma
  -mbmi2` (the `x86-64-v3` set `GENARCH` asks for) are accepted by clang-cl, and `GAMELIB_EXPORT`
  is already `__declspec(dllexport)` under `_WIN32`, so the generated C needs no changes — only
  a different compiler.
* The earlier note that clang-cl is "mandatory because of the System V guest ABI" (true for the
  gow3 port, whose guest code was already generated) does not apply here. This project *chooses*
  the ABI, and it chooses MSVC, because that is what the UWP toolchain can link.

---

## 4b. Rendering: there is no Vulkan surface on Xbox

The blueprint assumed the renderer would keep its Vulkan swapchain and get a surface from SDL.
Going to the source of the port that runs on a console says otherwise — `Cosmo/gow3_xbox`,
`xbox/graphics/present_bridge.cpp`:

```cpp
factory->CreateSwapChainForCoreWindow(queue, window.Get(), &desc, nullptr, &chain);   // D3D12
VkExternalMemoryImageCreateInfo  external{ .handleTypes = eD3D12Resource };
VkExportMemoryAllocateInfo       export_info{ .handleTypes = eD3D12Resource };
auto exported = device.getMemoryWin32HandleKHR({ .memory, .handleType = eD3D12Resource });
native->OpenSharedHandle(exported, IID_PPV_ARGS(&source));                           // D3D12 resource
```

No `VkSurfaceKHR`, no `VkSwapchainKHR`, no `vkAcquireNextImageKHR`, no `vkQueuePresentKHR`
anywhere in their presenter: `image_count = 1`, `Acquire()`/`Present()` are their own semaphore
wrappers. A frame is one `CopyResource` into the back buffer, then `Present`.

An AppContainer has no HWND, so this is not a preference — there is no Vulkan WSI surface to
create in the first place.

**What that means for this port.** SkyRecomp's entire present path is five call sites in one file:

| `runtime/nvn/vulkan/device.c` | today | on Xbox |
|---|---|---|
| 776 | `SDL_CreateWindow` | not created: the host owns the CoreWindow |
| 785 | `SDL_Vulkan_CreateSurface` | no surface |
| 320 | `vkCreateSwapchainKHR` | one shared image, `uwp_present_attach_vulkan_image` |
| 394, 398 | `vkAcquireNextImageKHR` | always the same image |
| 467 | `vkQueuePresentKHR` | `uwp_present_frame` |

Both halves are written and gated:

* `platform/uwp_present.{h,cpp}` — the D3D12 side. Takes an NT handle, so it stays free of Vulkan
  headers.
* `platform/uwp_vk_present.{h,c}` — the Vulkan side. Creates the image and its exportable
  dedicated allocation, exports the handle, and moves ownership in and out of D3D12 with explicit
  barriers (`begin` / `end`). Each barrier is its own submission with its own fence rather than a
  semaphore threaded through the renderer's timeline.
* The host hands the CoreWindow over in `SetWindow`, which is the moment it exists.

**Patch 0006** does the renderer side: `device.c` keeps its frame model and its `sc_images[]` /
`sc_views[]` arrays, but under `SKYRECOMP_UWP` the swapchain is one shared image, acquire is
`uwp_vk_present_begin`, present is `uwp_vk_present_end`, and the SDL window and surface are never
created. Six guarded regions, 82 lines. Two things it also had to handle, which surface-driven
code hides:

* `pick_device()` asks every queue family whether it can present to the surface — with no surface
  that is a null handle, so the console branch treats every family as presentable and lets the
  bridge submit to the queue D3D12 presents from.
* `create_device()` requested `VK_KHR_swapchain`. On the console it requests
  `VK_KHR_external_memory_win32` instead and **fails loudly** when the driver lacks it, because
  without it there is no way to hand an image to D3D12. (`VK_KHR_external_memory` and
  `VK_KHR_dedicated_allocation` are core in Vulkan 1.1, which is the floor here.)

The bridge and the renderer also had to agree on a layout: the renderer leaves the window image in
`COLOR_ATTACHMENT_OPTIMAL`, so both ownership transfers declare that, not `GENERAL`. D3D12 ignores
Vulkan layouts, so this only has to be self-consistent — but it has to be. Dozen has to expose `VK_KHR_external_memory`,
`VK_KHR_external_memory_win32` and `VK_KHR_dedicated_allocation`; gow3 calling
`vkGetMemoryWin32HandleKHR` on the console is the evidence that it does.

**Consequence for SDL, and it is a large one.** With no window and no surface, the SDL *video*
driver is off the critical path entirely. The runtime still uses SDL for the event queue,
gamepads, gyro, audio streams and timing (219 distinct symbols), but those are the joystick,
sensor, audio and timer subsystems — not `src/video/winrt/`. That turns the dependency from
"port SDL2's WinRT app model and video backend into SDL3" into "build SDL3 for the APP partition,
minus video, and patch what breaks", which is a different order of effort. It has not been
verified by building SDL3 that way; that is the next thing to measure, not to assume.

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
| `uwp.c` **compiles** | **yes** — clang-cl `/fsyntax-only`, `WINAPI_FAMILY_APP` + `_CRT_APP` + `SKYRECOMP_UWP`, run `41052945145` green, 0 diagnostics |
| `uwp.c` and `uwp_entry.cpp` **compile** | **yes** — clang-cl and `cl /ZW` in CI, run `41056498090` green |
| `uwp.c` **runs on a console** | **no — never run** |
| `uwp_present.cpp`, `uwp_vk_present.c` **compile** | **yes** — clang-cl, run `41058121314` green |
| the bridge **works on a device** | **no** — external memory, the ownership transfers and the copy are all unexercised |
| Dozen/SDL3/FFmpeg link | **no — none of them built for UWP yet** |

The first real compile has now happened, and it was worth having: six errors and five
warnings, all of them real. Two APIs the platform layer wanted (`GetCurrentPackagePathW`,
`MoveFileA`) are **not declared in the APP partition of this SDK**, even when their documented
headers are included, so both dependencies were removed rather than worked around. A third,
`AddVectoredExceptionHandler`, cannot be removed - lazy commit of the guest window is a vectored
exception handler - so its prototype is declared locally with the reason recorded there.

Behavioural guesses are still guesses: everything marked `UWP-ASSUMPTION` compiles, and none
of it has been executed. Grep for the marker.

---

## 9. Remaining work, in order

1. ~~Build `uwp.c` with clang-cl on Windows~~ **Done** — `compile-uwp-platform.yml`, green.
   The gate is now permanent: any change here that breaks the UWP configuration fails CI.
2. ~~Write `port/runtime/platform/uwp_entry.cpp`~~ **Done** — `SkyRecomp.Uwp.App`, C++/CX,
   checked by `cl /Zs /ZW` in the same CI job. It resolves LocalState and the install location
   with WinRT, refuses a sub-2 GiB budget with the Dev Home fix, and runs the runtime on the
   view's `Run()` thread. What it does *not* do yet is hand the CoreWindow to the renderer: that
   waits on the SDL3 WinRT backend (step 4), which is what will own the window.
3. **Build Dozen** (`vulkan_dzn.dll`) and `dxil.dll` for UWP. Fork of Mesa, `-Dvulkan-drivers=
   microsoft`, D3D12, DXIL 1.7. Largest single task.
4. **Build SDL3 3.4.x for the APP partition without video** (section 4b). The version floor is
   not a preference: `device.c`'s screenshot path calls `SDL_SavePNG`, which does not exist in
   3.2 — the compiler said so, out loud, the first time the renderer was compiled. The video backend — the large
   piece of section 7 — is not needed: there is no window and no surface. What must work is the
   event queue, gamepads, sensors, audio and timing. Size it by building it, not by estimating.
5. **Build FFmpeg (LGPL) and shaderc for UWP**, static.
6. ~~`Package-Uwp.ps1` with `skygame.dll` + `skygame.dll.id` in the layout~~ **Done** —
   `-GameDir` stages both at the package root (where `LoadPackagedLibrary` looks), refuses a
   package whose `.id` is missing or malformed, and refuses to pretend: without the switch the
   package is labelled a smoke test in `INSTALL.md` and in the artifact's `has_game_code` output.
   What still has to happen is the first half of that sentence: `make gen && make` on a PC with
   the game files set up, then the CMake project.
7. **First frame on the console**, then the memory pager if and when the budget bites.

Steps 2-8 now have an ordered, per-step recipe in `BUILD-PORT.md`, with each command's
provenance and an explicit "not yet run" mark where nobody has executed it. That file did not
exist until it was noticed that `CMakeLists.txt`'s own fatal error told the reader to go read it.

Steps 1–5 all need a Windows machine with the UWP workload. They can run on GitHub Actions
`windows-latest` (which already has VS 17.14 + both SDKs, proven by the smoke-test pipeline), but
the game dump has to be supplied by the person building — it cannot be in a public CI job, and it
must not be.
