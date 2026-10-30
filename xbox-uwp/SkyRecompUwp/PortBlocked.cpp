// PortBlocked.cpp
//
// This translation unit exists to make a blocked build fail loudly and explain itself.
//
// The project around it (SkyRecompUwp.vcxproj) is the intended home of the Xbox UWP port
// of the SkyRecomp runtime. It contains no runtime sources yet because four blockers have
// to be solved first, and each of them changes the shape of the app rather than a flag:
//
//   1. RENDERER. runtime/nvn/vulkan/ is 6,452 lines of Vulkan 1.3. Xbox exposes Direct3D
//      only (D3D11 FL10.0/11.0, D3D12 FL11.0 + SM 6.4). A D3D12 backend has to replace it.
//      See ../../XBOX-UWP-PORT-PLAN.md, Stage 1.
//
//   2. PROCESS MODEL. runtime/platform/windows.c uses CreateProcessW (4 call sites),
//      CreateJobObjectW, and re-execs itself; runtime/gamebuild.c runs a bundled
//      Zig 0.17.0 compiler on the player's machine to build skygame.dll. Both are
//      impossible inside a UWP AppContainer. The game library must be built off-console.
//      See Stage 3.
//
//   3. MEMORY. runtime/rt.h claims a 12.5 GB guest window at 0x70_0000_0000. An Xbox UWP
//      app in Game resource mode is limited to 5,120 MiB of unified RAM+VRAM. The guest
//      working set plus host-side texture copies does not fit without an eviction scheme.
//      See Stage 4.
//
//   4. STORAGE. The launcher picks an arbitrary folder, writes mods/saves/logs anywhere,
//      and shell-opens folders. UWP requires brokered storage. See Stage 2.
//
// Remove this file only when at least one real runtime source compiles here, and delete
// the #error below in the same commit.

#error "SkyRecompUwp is scaffolded but not ported. See xbox-uwp/SkyRecompUwp/README.md, ../../BUILD_NOTES.md section 4, and ../../XBOX-UWP-PORT-PLAN.md. Nothing here can produce a working Xbox package yet."
