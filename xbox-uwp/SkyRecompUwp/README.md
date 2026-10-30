# SkyRecompUwp — the gated real-runtime project

**This project does not build. That is deliberate, and it is not a defect in the scaffold.**

`PortBlocked.cpp` fails the build with an explicit `#error` that names the four things
standing between the SkyRecomp runtime and an Xbox package. The alternative — a project
that "succeeds" by silently excluding the renderer, the platform layer and the game
library — would produce a package that installs, launches, and shows nothing. A loud
build failure is the honest signal.

CI never builds this project. It is enabled only when someone deliberately sets the
repository variable `ENABLE_UWP_RUNTIME_BUILD=true` (see `.github/workflows/build-xbox-uwp.yml`),
which is a marker that the port work below has actually landed.

## What has to change before this project can produce a package

The full analysis is in `../../BUILD_NOTES.md` §4 and `../../XBOX-UWP-PORT-PLAN.md`.
Summary, mapped to files:

| # | Blocker | Files | Status |
|---|---|---|---|
| 1 | Xbox has no Vulkan. `runtime/nvn/vulkan/` (6,452 lines) must become a D3D12 backend | `runtime/nvn/vulkan/*`, `Makefile` | not started |
| 2 | `CreateProcessW` / job objects / on-device `zig cc` compilation are illegal in AppContainer | `runtime/platform/windows.c`, `runtime/gamebuild.c` | not started |
| 3 | 12.5 GB guest window vs. the 5,120 MiB Xbox UWP memory ceiling | `runtime/rt.h`, `runtime/main.c`, `runtime/nvn/texpatch.c` | not started |
| 4 | Free-form file access (folder picker, `ShellExecute`, arbitrary write paths) must become brokered storage | `runtime/platform/windows.c`, `runtime/ui/launcher.mm`, `runtime/mods.c` | not started |

## What this project already gets right

- **Project shape**: `AppContainerApplication=true`, `ApplicationType=Windows Store`,
  `ApplicationTypeRevision=10.0`, C++/CX (`CompileAsWinRT`), `v143` toolset, x64 only.
  This is the same configuration Microsoft's own Xbox samples use.
- **SDK pin**: `WindowsTargetPlatformVersion=10.0.26100.0`, overridable from the command line.
- **Static CRT** (`/MT`): no `Microsoft.VCLibs` framework dependency in the deployed package.
- **No full-trust**: `rescap:runFullTrust` is absent on purpose. It is not permitted on Xbox.

## When you actually start the port

1. Start with Stage 0 of `../../XBOX-UWP-PORT-PLAN.md` (feasibility gate), not with code.
   The memory question alone can end the project cheaply if the answer is bad.
2. Add sources to this project in the same groups the Makefile uses, so the two builds
   stay comparable: `runtime/*.c`, `runtime/hle/*.c`, `runtime/nvn/d3d12/*`, `runtime/ui/*.mm`.
   Use `CompileAsWinRT=false` for plain C translation units — only the entry point and
   anything touching WinRT types needs `/ZW`.
3. Keep `SourceLink`-style discipline: one `PLATFORM=uwp` equivalent, no forked copies of
   runtime sources. If a file needs Xbox-specific behaviour, `#if defined(SKYRECOMP_UWP)`
   inside the shared file, never a duplicate.
4. Delete `PortBlocked.cpp` in the same commit that makes the first real source file compile,
   and drop the `#error` — but only then.
