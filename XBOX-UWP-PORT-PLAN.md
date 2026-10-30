# SkyRecomp → Xbox Series X|S (UWP, Dev Mode): port plan

Companion to `BUILD_NOTES.md`. This is the engineering work required before an `xbox-uwp.zip` can exist. Nothing here is done yet; the estimates are order-of-magnitude, for scoping only.

Target: **UWP/MSIX, x64, Xbox Series X|S, Developer Mode**. Not retail, not GDK, not Xbox One (see the AVX2 note in Stage 2).

> **REVISED 2026-10-30 after studying five shipping Dev Mode ports.**
> Stage 1 below is **superseded**: the console does run Vulkan, via Mesa's **Dozen**
> (Vulkan → D3D12) driver, and two shipped Zelda-class recompilations do exactly that. See
> [`xbox-uwp/PORT-BLUEPRINT.md`](xbox-uwp/PORT-BLUEPRINT.md) for the evidence and the revised,
> cheaper task list. The blockers and Stages 2–5 remain valid and are refined there.

---

## Stage 0 — Feasibility gate (half a day, must pass first)

Kill criteria before writing any code:

| Question | How to answer |
|---|---|
| Does D3D12 FL11.0 + SM 6.4 cover the pipeline features `runtime/nvn/vulkan/*.c` actually uses? (dynamic rendering, descriptor indexing, timeline semaphores, FSR/SMAA passes, ASTC paths) | Enumerate every `vkCmd*`/`vkCreate*` call in `exec.c`, `device.c`, `textures.c`, `shaders.c`; map each to a D3D12 equivalent; list the misses |
| Can the guest memory fit 5,120 MiB? | Instrument a desktop run: `AppMemoryUsage`-equivalent commit peak of `skyrecomp.exe` minus GPU allocation, on a scene-heavy save. If committed > ~4.5 GiB on desktop, the port needs a memory architecture change, not tuning |
| Is there any Xbox-legal path for the setup step? | Confirm the game library can be produced entirely off-console (Stage 3), since `zig cc` cannot run in the sandbox |
| Is SDL3 3.4.18 usable under AppContainer/Xbox? | Check upstream SDL3 for a WinRT/Xbox backend at that tag; if absent, budget a platform layer (Stage 2) |

If the memory gate fails hard, the honest output is "not portable to Dev Mode at playable settings with current assets" — say so rather than shipping a package that OOMs on boot.

---

## Stage 1 — Renderer: **SUPERSEDED**, use Mesa Dozen (~1–2 weeks)

> **Do not write a D3D12 backend from scratch.** Both `Cosmo/gow3_xbox` and `JYRS20/WWHD-UWP`
> ship `vulkan_dzn.dll` (Mesa Dozen, Vulkan-on-D3D12) plus `dxil.dll`, and run their existing
> Vulkan renderers unchanged on the console. The original reasoning below is kept only to
> record what it got wrong.

**Original Stage 1 (kept for the record):**

Replace `runtime/nvn/vulkan/` with `runtime/nvn/d3d12/` behind the existing renderer interface.

| Existing file | Lines | D3D12 counterpart |
|---|---|---|
| `runtime/nvn/vulkan/device.c` | 854 | device/queue/fence factory, DXGI swapchain or Xbox present path, descriptor heaps |
| `runtime/nvn/vulkan/exec.c` | 3,900 | command translation: Maxwell pushbuffer → PSO + root signature + barrier state machine |
| `runtime/nvn/vulkan/shaders.c` | 529 | replace shaderc GLSL→SPIR-V with SPIR-V→DXIL (Mesa `spirv_to_dxil`) + `dxil.dll` signing, per-device DXIL cache on disk |
| `runtime/nvn/vulkan/textures.c` | 930 | upload/copy/format translation, swizzle and ASTC handling identical to the Xbox-side projects' BC1/BC3 approach |
| `runtime/nvn/vulkan/vma.cpp` | 9 | D3D12 heaps + a residency/budget manager instead of VMA |
| `runtime/nvn/vulkan/imgui_glue.cpp` | 100 | `imgui_impl_dx12` |
| `runtime/nvn/smaa_data.c`, `output_scale.c` | — | portable, but the SMAA/FSR passes need D3D12 pipeline variants |
| `runtime/platform/windows.c` (Vulkan loader init) | 477 | remove loader entirely; D3D12 is in-OS |

Also: `Makefile` gains a `PLATFORM=uwp` branch (or an MSBuild/CMake project is added — the Makefile will not survive UWP tooling).

Reference implementations to study, same problem, same console: `JulianDr14/eden-xbox`, `PrismFox67/Eden-UWP`.

---

## Stage 2 — UWP platform layer (~1–3 weeks)

- **Entry point**: `WinMain` in `runtime/platform/windows.c` → UWP `App` + `CoreWindow`/`ApplicationView` activation. Keep the existing logic behind `runtime/platform/uwp.c`.
- **Process model** (`CreateProcessW` ×4, `CreateJobObjectW`, console attach): delete child-process use. Split logging to `ApplicationData.LocalFolder`, drop the "open console" diagnostics path, drop the self re-exec used for machine-class detection.
- **File system** (`runtime/platform/windows.c`, `runtime/ui/launcher.mm`, `mods.c`, save paths): replace `fopen` on arbitrary paths with a brokered storage layer — package-content assets for read-only data, `ApplicationData.Current` for saves/logs/caches, and a folder library chosen through `FolderPicker` for user-supplied game files. `ShellExecute` folder-open for the mod manager must become a storage-based listing (no shell on console).
- **Input**: SDL3 gamepad paths (`runtime/hle/input.c`, `bindings.c`) — no keyboard/mouse assumptions; the on-screen keyboard (`runtime/ui/osk.mm`) is the only text entry.
- **Audio**: verify the SDL3 audio backend under AppContainer (WASAPI UWP surface); Xbox-side projects use XAudio2 2.9 as the known-good fallback.
- **Threads/affinity**: Zen 2 in the Series consoles — 7 cores usable in Game mode; `frame_rate.c` and the job system need a core budget rather than `nproc`.
- **AVX2**: `GENARCH ?= -march=x86-64-v3` is correct for Series X|S (Zen 2, AVX2+FMA) and wrong for Xbox One (Jaguar). Decide the target explicitly and encode it in the manifest's minimum requirements.
- **SDL3**: pin the exact tag that is verified under UWP; write down the verification (a hello-triangle inside the sandbox), because a guess here poisons every later debug session.

---

## Stage 3 — Game library without an on-device compiler (~1 week)

Today `runtime/gamebuild.c` invokes bundled `zig cc` at setup time to build `skygame.dll`, and `launcher.mm:277` instructs the user to keep `zig` next to the exe. In AppContainer that is impossible.

- Produce `skygame.dll` (or a static-linked variant) **off-console**, from the user's own game files, on a PC: `make gen` → `skygame.dll` with the UWP ABI/CRT.
- Ship the recompiled library as part of the package payload, or import it through a brokered storage folder and load with `LoadPackagedLibrary` (which requires it to be in package content, so: package payload).
- Validate `GAMELIB_ID` (`Makefile:118`) still catches version skew between runtime and library.
- Legal note: the library is derived from the user's own game files, so it must be generated **by the user on their own PC**, never distributed with the package.

---

## Stage 4 — Memory budget (~1–2 weeks, after Stage 1)

- Replace the 12.5 GB guest window claim (`runtime/rt.h:15`, `main.c`) with a placeholder-reservation + commit-on-demand scheme, and cap committed memory against `MemoryManager.AppMemoryUsage` every frame.
- Eviction policy for texture caches (`runtime/nvn/texpatch.c`, custom texture packs) and pipeline caches (PSOs are the classic Xbox OOM source).
- Add a "safe" mode: no 4K texture pack, reduced output scale, smaller caches. The 4K pack is unlikely to fit in 5,120 MiB unified RAM+VRAM alongside ~3.2 GiB of guest data — plan for it to be unavailable or streamed.

---

## Stage 5 — Packaging & signing (~2–3 days, needs Windows)

```
xbox-uwp/
  SkyRecomp.UWP.sln
  SkyRecomp.UWP/            # C++ UWP app project (x64 only)
    Package.appxmanifest    # TargetDeviceFamily Windows.Desktop + Windows.Xbox, x64
    Assets/                 # Square150x150Logo, Square44x44Logo, StoreLogo, SplashScreen
  SkyRecomp.UWP.Package/    # Windows Application Packaging Project (wapproj)
  build-xbox-uwp.ps1        # msbuild + makeappx + signtool, deterministic
```

- Manifest: `TargetDeviceFamily` for `Windows.Xbox` (`MinVersion="10.0.19041.0"`-class), `ProcessorArchitecture="x64"`, capabilities `internetClient` (if any), `removableStorage`/`broadFileSystemAccess` **not** usable on console — use the folder-broker flow instead.
- Sign with a self-signed code-signing cert whose **subject exactly matches `Identity Publisher`**; ship the `.cer` next to the package.
- Package as `.msix` (or `.appx`), then zip: `xbox-uwp.zip` containing `*.msix` + `*.cer` + `dependencies/` if needed + `SHA256SUMS.txt`.
- CI: a `windows-latest` GitHub Actions job that builds, packages, signs (cert from secrets), and emits the zip + SHA256 as a release asset. That is the only way this stays reproducible.

---

## Total

Revised estimate: **6–10 engineer-weeks** (was 8–14). The renderer stops being the long pole;
memory paging against the 5,120 MiB budget becomes the dominant risk instead.

Two claims above are now **wrong** and corrected in `xbox-uwp/PORT-BLUEPRINT.md`:

- *"the console has no Vulkan"* — it has no **native** Vulkan, but Mesa Dozen provides it over
  D3D12, and shipped ports use it.
- *"SDL3's UWP status is unverified"* — SDL3 has a WinRT video backend and a Windows.Gaming.Input
  joystick path; two small patches make it work on console, pinned at the same 3.4.18 tag
  SkyRecomp already builds.

What remains true: there is no full-trust escape hatch, so a wrapper or a repackaging of the
desktop build still cannot work.
