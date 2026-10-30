# Port blueprint: what the working Xbox ports actually do

Synthesis of the six sources you gave me, and what each one changes about the SkyRecomp plan.
Written 2026-10-30 against upstream `Apolinario-coder/SkyRecomp` @ `f005a619`.

**The short version: my earlier plan was wrong about the biggest item.** I said Xbox had no
Vulkan and the runtime needed a 6,452-line D3D12 backend written from scratch. Both sibling
ports in your list run **Vulkan on the console** by shipping Mesa's **Dozen** driver
(Vulkan → Direct3D 12). That turns a multi-week renderer rewrite into an integration task.

---

## 0. Which source teaches what (read this first)

| Source | What it is | Relevance here |
|---|---|---|
| [devdocs / paths/porting/from-pc](https://devdocs.xbox.com/paths/porting/from-pc) | **Official Microsoft GDK** porting path: GDK, MSIXVC packaging, Partner Center, ID@Xbox, certification | **Not the path you're on.** Requires GDK access (NDA) and produces retail/MSIXVC packages. Does not apply to Developer Mode sideloading. Useful only as the "if you ever go legit" roadmap. |
| [Cosmo/gow3_xbox](https://github.com/Cosmo/gow3_xbox) | God of War III (PS4 recomp) on **Series X\|S, Developer Mode** | **The best engineering reference.** CMake + clang-cl, Dozen, a guest-memory **pager**, desktop shim layer, shared signing key, release CI. |
| [JYRS20/WWHD-UWP](https://github.com/JYRS20/WWHD-UWP) | Zelda: Wind Waker HD (Wii U recomp) on **Xbox, Developer Mode** | **The closest analogue to SkyRecomp**: same genre lineage, SDL3 + Vulkan, and it compiles the user's game **on the console**. Proven playable ~30 FPS on Series S by user testing. |
| [XboxEmulationHub/dusklight-uwp](https://github.com/XboxEmulationHub/dusklight-uwp) | TP recomp, UWP fork | **Discontinued upstream.** Little to copy beyond confirming the general shape. |
| [YTReviveMe/melee-UWP](https://github.com/YTReviveMe/melee-UWP) | Smash Melee UWP | **Repository returns 404** — private, renamed or deleted. Nothing to study. |
| [YTReviveMe/warthog-UWP](https://github.com/YTReviveMe/warthog-UWP) | Halo CE for the **original Xbox** (dev kits / modded consoles) | **Not a UWP port at all.** No Dev Mode, no AppContainer, no MSIX. Irrelevant to this task, though its own README is honest that it has no release yet. |

So: **two** of the six are directly useful, and they agree with each other on almost every
point. Everything below is drawn from those two plus the official docs for terminology.

---

## 1. The correction that matters: Vulkan on the console via Mesa Dozen

**What I claimed before:** "Xbox exposes Direct3D only, so `runtime/nvn/vulkan/` (6,452 lines)
must be rewritten in D3D12 — 4–8 engineer-weeks."

**What the ports actually do:** they ship `vulkan_dzn.dll` — Mesa's **Dozen** driver, which
implements Vulkan on top of D3D12 — plus `dxil.dll` from Microsoft's DirectX Shader Compiler
for shader signing. The existing Vulkan renderer then runs unchanged.

Evidence:

- gow3 `CREDITS.md`: *"**Mesa / Dozen**, Vulkan on Direct3D 12, from the fork
  `https://github.com/Cosmo/mesa_ai_slop_version` (`vulkan_dzn.dll`). MIT."* and
  *"**DirectX Shader Compiler** (`dxil.dll`) by Microsoft."*
- gow3 `xbox/CMakeLists.txt`:
  ```cmake
  set(GOW3_DOZEN_DLL "${GOW3_MESA_ROOT}/build/dozen-uwp/src/microsoft/vulkan/vulkan_dzn.dll"
      CACHE FILEPATH "Dozen built for UWP")
  set(GOW3_DXC_ROOT "${GOW3_MESA_ROOT}/tools/dxc" CACHE PATH "DirectX Shader Compiler release")
  target_compile_definitions(gow3_uwp PRIVATE GOW3_BUILD_GRAPHICS VK_USE_PLATFORM_WIN32_KHR)
  target_link_libraries(gow3_uwp PRIVATE Vulkan::Headers d3d12 dxgi)
  ```
- WWHD README: *"Vulkan rendering through **Mesa Dozen / Direct3D 12** and UWP input, storage
  and lifecycle support."* Its Mesa patch set is small: `meson.build` python detection, an
  added `uwp` meson option, and a DXIL validator version bump in
  `src/microsoft/compiler/dxil_validator.cpp`.

**Consequences for SkyRecomp:**

| Before | Now |
|---|---|
| New D3D12 backend, ~6,500 lines | Build Dozen for UWP from the Mesa fork; ship `vulkan_dzn.dll` + `dxil.dll` |
| Replace `vkCmd*`/PSO/descriptor code | Keep `runtime/nvn/vulkan/*` as-is |
| 4–8 weeks | 1–2 weeks integration + whatever Dozen feature gaps surface |

**Known Dozen gaps to expect** (from the gow3 sources, which carry workarounds):
`descriptor_fallback.cpp`, `dense_buffers.cpp`, `texture_probe.cpp`, `present_bridge.cpp`.
Translation: Dozen does not cover every Vulkan 1.3 feature, and SkyRecomp's renderer uses
descriptor indexing, dynamic rendering and FSR/SMAA passes. Budget for per-feature fallbacks,
not a rewrite.

---

## 2. Build system: what actually compiles for UWP

Both ports use **CMake driving clang-cl**, not the MSBuild `.vcxproj` I scaffolded. That is the
single biggest practical lesson: the Makefile project is C and clang-shaped, and clang-cl keeps
the System V x64 ABI and `musttail` that recompiled code depends on.

WWHD `platforms/uwp-game/CMakeLists.txt`:

```cmake
set(CMAKE_SYSTEM_NAME WindowsStore)      # the CMake UWP switch
set(CMAKE_SYSTEM_VERSION 10.0)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "The game runtime requires Clang's musttail support")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)   # /MD — matches my D8016 finding
add_compile_definitions(WINAPI_FAMILY=WINAPI_FAMILY_APP _CRT_APP ... NOMINMAX)
link_directories(BEFORE "$ENV{VCToolsInstallDir}/lib/x64/store")   # Store CRT import libs
```

gow3 `xbox/CMakeLists.txt`:

```cmake
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use clang-cl: guest code uses the System V x64 ABI")
endif()
target_link_options(gow3_uwp PRIVATE /APPCONTAINER /HIGHENTROPYVA:NO /STACK:16777216,16777216 /OPT:REF)
target_link_directories(gow3_uwp BEFORE PRIVATE "$ENV{VCToolsInstallDir}/lib/x64/store")
target_link_libraries(gow3_uwp PRIVATE WindowsApp "${GOW3_PTHREAD}/lib/libwinpthread.dll.a")
```

Two flags matter enormously for SkyRecomp specifically:

- **`/HIGHENTROPYVA:NO`** — the runtime claims a fixed 12.5 GB guest window at
  `0x70_0000_0000` (`runtime/rt.h`). High-entropy VA randomisation would scatter host
  allocations through that range and break the reservation, exactly as gow3 documented
  ("UWP host allocations already occupy part of that interval").
- **`/STACK:16777216,16777216`** — the existing Windows Makefile already sets a 16 MB stack;
  the recompiled guest code needs it.

---

## 3. Memory: the 5,120 MiB wall is real, and it is solved by paging

Both ports confirm the number and ship a workaround:

- gow3 README: *"In Developer Mode an app gets at most 5 GiB. At 4K the app moves about
  450 MiB of the game's memory to disk and runs at about 45 fps; at 720p nothing goes to disk."*
- gow3 milestone log: *"Series X reports a 5,120 MiB app-memory budget"*, and later
  *"combat at approximately 4.0 GiB"*.
- gow3 `xbox/platform/guest_swap.inc` — the pager itself. `CREDITS.md` credits
  *"KytyPS5 for Xbox … the Xbox memory pager"* as the reference.

For SkyRecomp this is the plan for Blocker B3: keep the 12.5 GB **reservation** (virtual, cheap)
but make **commit** paged — evict guest pages to disk under pressure, with a per-frame budget
check. The `guest_swap.inc` design and KytyPS5's pager are the two things to read before writing
one. Also relevant: WWHD's changelog mentions *"reduced guest-memory commitment"* and *"AVX2
texture checks"* — the same class of tuning.

---

## 4. Storage: where a UWP app may actually read and write

This is the part that silently breaks ports, and gow3 documents it precisely:

> *"the console's internal storage, anywhere under **`D:\DevelopmentFiles`** … This is the Dev
> Mode folder that apps may read; other places on the internal storage are closed to apps."*

Plus:

- **USB** works, declared via `<uap:Capability Name="removableStorage" />`, browsed with a
  `FolderPicker` and remembered across launches.
- **`ApplicationData.LocalFolder`** (`LocalState`) is the only reliable private write location —
  WWHD puts prepared game data in `LocalState/WWHD/Games`.
- Neither port uses `broadFileSystemAccess`; it is not available on console.

Mapping to SkyRecomp's Blocker B4: the launcher's free-form folder picker becomes
(a) `DevelopmentFiles` / USB via picker for the user's game files, (b) `LocalState` for
`skyrecomp.log`, saves, mods and texture packs, (c) package content for everything read-only.

---

## 5. The on-device compile problem (SkyRecomp's Blocker B2)

SkyRecomp builds `skygame.dll` on the player's machine with a bundled **zig 0.17.0**
(`runtime/gamebuild.c`, `tools/fetch_zig.sh`) and shells out with `CreateProcessW`. In an
AppContainer that is illegal. The ports show **two working answers**:

1. **Compile on the console with embedded clang.** WWHD ships `platforms/uwp-clang/` — a full
   UWP clang build (`patches/llvm-20.1.8-uwp.patch`) with `compile_host.cpp` and
   `clang_api.h`, exposed to the app through `wwhd-clang-bridge.dll`
   (`WWHD_CLANG_BUILD`, `add_library(gamecode SHARED IMPORTED)`). Its README: *"Extract,
   translate, compile and verify your own game directly on the console."*
2. **Compile off-console and load the result from USB.** Same repo, `WWHD_DYNAMIC_GAMECODE`:
   `add_library(gamecode SHARED IMPORTED)` with an import lib, `WWHD_EXTRACT_UWP=1`, and
   *"Dynamic loading of verified game objects from USB; the APPX contains no generated game
   instructions"* (changelog 0.1.2).

For SkyRecomp, **option 2 is much cheaper**: keep the existing toolchain (`zig cc` / clang) on a
PC, produce the game library off-console, and load it in the sandbox. The catch is that
`LoadPackagedLibrary` only loads from package content — so either the library ships in the
package (can't: it is derived from the user's own game) or you use an equivalent of WWHD's
bridge DLL. That bridge is the thing to read first, and it is the real answer to B2.

Legally this is also the cleanest position, and both ports state it explicitly: the APPX
contains no generated game instructions, and preparation happens from the user's own copy.

---

## 6. SDL3 on UWP: proven, with two small patches

My Blocker B5 said SDL3's UWP status was unverified. It is verified now:

- SDL3 has a **WinRT video backend** (`src/video/winrt/SDL_winrtvideo.cpp`) and a
  **Windows.Gaming.Input** joystick path (`src/joystick/windows/SDL_windows_gaming_input.c`).
- WWHD builds SDL3 for UWP separately with MSVC and imports it:
  ```cmake
  add_library(SDL3_UWP SHARED IMPORTED)
  set_target_properties(SDL3_UWP PROPERTIES
      IMPORTED_IMPLIB "${WWHD_SDL_BUILD}/SDL3.lib"
      IMPORTED_LOCATION "${WWHD_SDL_BUILD}/SDL3.dll"
      INTERFACE_INCLUDE_DIRECTORIES "${WWHD_SDL_SOURCE}/include")
  ```
- Two patches are needed, both small and both relevant to SkyRecomp's controller code
  (`runtime/hle/input.c`, `bindings.c` rely on `SDL_GamepadType`):
  - `sdl-pixel-format.patch` — return type of `D3D11_DXGIFormatToSDLPixelFormat` is
    `SDL_PixelFormat`, not `Uint32`.
  - `sdl-wgi-input.patch` — on Xbox, WGI must report the **raw** button layout
    (`nbuttons = 14`) so `SDL_CreateMappingForWGIGamepad` maps it; the stock code assumes
    `SDL_GAMEPAD_BUTTON_COUNT` and gets the wrong layout.
- The repo pins SDL3 **3.4.18**, the same tag SkyRecomp's CI uses — so the patch set should
  transplant directly.

---

## 7. Packaging and signing, exactly as the ports do it

**Manifest** (both ports agree, and it differs from my scaffold):

```xml
<TargetDeviceFamily Name="Windows.Universal" MinVersion="10.0.19041.0" MaxVersionTested="10.0.26100.0" />
<PackageDependency Name="Microsoft.VCLibs.140.00" MinVersion="14.0.33519.0"
                   Publisher="CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" />
<Capabilities>
  <Capability Name="internetClient" />
  <Capability Name="codeGeneration" />                    <!-- gow3 -->
  <uap:Capability Name="removableStorage" />              <!-- USB -->
  <rescap:Capability Name="extendedExecutionUnconstrained" />  <!-- WWHD: no suspension -->
</Capabilities>
```

Differences from my scaffold worth adopting: `Windows.Universal` rather than `Windows.Xbox`;
the **VCLibs `PackageDependency` declared in the manifest** instead of shipping the appx for
manual installation; `codeGeneration`; `removableStorage`; and
`extendedExecutionUnconstrained` (a game that gets suspended mid-frame is not acceptable).

**Signing:** gow3 uses a *deliberately public* self-signed key:

> *"Packages are signed with `signing/gow3_xbox-signing.pfx`, a self-signed test key for
> `CN=gow3_xbox` that is public on purpose: it vouches for nobody, and with one shared key every
> build, from any machine or from GitHub, installs over the previous one."*

That is a good idea I should copy: one stable publisher identity per project, so every release
installs over the last, and CI can sign without secrets. My current workflow mints a new
certificate per run, which means a new publisher trust each time.

**CI shape** (gow3 `.github/workflows/xbox.yml`): build Dozen from the Mesa fork → build the
launcher → produce `GodOfWar3Launcher-<version>.zip` → publish a pre-release with install steps.
Mine already does everything except the Dozen step.

---

## 8. Revised plan for SkyRecomp

Supersedes Stages 1–4 of `XBOX-UWP-PORT-PLAN.md`.

| # | Work | Effort | Blocked on |
|---|---|---|---|
| 1 | **CMake + clang-cl UWP project** replacing the `.vcxproj` scaffold: `CMAKE_SYSTEM_NAME=WindowsStore`, `/APPCONTAINER /HIGHENTROPYVA:NO /STACK:16MB`, Store CRT lib dirs, `WindowsApp.lib`, winpthreads | 3–5 days | — |
| 2 | **Dozen**: build `vulkan_dzn.dll` for UWP from a Mesa fork, ship with `dxil.dll`; point the existing Vulkan renderer at it | 1–2 weeks | #1 |
| 3 | **SDL3 3.4.18 for UWP** + the two WWHD patches; verify gamepad mapping against `runtime/hle/input.c` | 3–5 days | #1 |
| 4 | **Platform layer**: replace `CreateProcessW`/job objects with in-process calls; `LocalState` for logs/saves/mods; `DevelopmentFiles`+USB via picker for game files; `extendedExecutionUnconstrained` lifecycle | 1–2 weeks | #1 |
| 5 | **Game library off-console + bridge load** (WWHD `wwhd-clang-bridge` pattern) instead of on-device `zig cc` | 1–2 weeks | #4 |
| 6 | **Memory**: paging/eviction against the 5,120 MiB budget, using `guest_swap`/KytyPS5 pager as reference | 2–4 weeks | #2 |
| 7 | **Package + CI**: manifest per §7, one shared signing key, Dozen build in the workflow | 3–5 days | #1 |

**Realistic total: 6–10 engineer-weeks**, down from my earlier 8–14, and with a much smaller
chance of a dead end — because every risk above now has a shipped, working precedent on the
same console, and two of them are *Zelda recompilations*.

The honest caveats: I read these repositories' READMEs, manifests, CMake files, patches and
change logs — not their full source. Dozen's coverage of the specific Vulkan features
SkyRecomp uses is still unmeasured, and that is the one place this plan could still sink. That
measurement is Stage 0 and it is cheap: enable Dozen on a desktop Windows build and see what
the renderer does.
