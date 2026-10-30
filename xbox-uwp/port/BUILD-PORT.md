# Building the port

Everything here runs on a **Windows x64 machine with the UWP C++ workload**; nothing in this file
runs on the console. The result is `xbox-uwp.zip`, installed through Developer Mode sideloading.

Read this as a build order, not a wish list. Each section says what it produces, where the command
comes from, and **whether it has actually been run**. Sections marked *not yet run* are the honest
gaps: the flags come from the upstream source tree or from the two shipping ports this one is
modelled on, but nobody has executed them end to end yet.

    Verified   = executed, and it is in CI so it stays true
    Not run    = derived from the repo below, but never executed. Expect to fix something.

| # | Step | Produces | Status |
|---|------|----------|--------|
| 1 | Build the game code on a PC | `skygame.dll` + `skygame.dll.id` | **not yet run** |
| 2 | SDL3 3.4.18, APP partition | `SDL3.lib`, `SDL3.dll`, headers | **not yet run** |
| 3 | FFmpeg 9.0.2, LGPL, static | `libav*.lib`, headers | **not yet run** |
| 4 | shaderc, static | `shaderc_combined.lib`, headers | **not yet run** |
| 5 | doxil / Dozen | `vulkan_dzn.dll` | **not yet run** |
| 6 | DirectX Shader Compiler | `dxil.dll` | **not yet run** |
| 7 | Configure + build the port | `skyrecomp_uwp.exe` + `.winmd` | **not yet run** |
| 8 | Package | `xbox-uwp.zip` + SHA256 | **verified** (`build-xbox-uwp.yml`) |

Prerequisites, from `tools/setup_windows.ps1` in the upstream tree: Visual Studio 2022 with the
UWP C++ workload (`v143`), the Windows 11 SDK, LLVM/clang-cl, Python 3.12, and MSYS2 if you also
want to build the upstream desktop target. `msvc-dev-cmd` style environment (`vcvars64`) must be
active for steps 5–8.

---

## 1. The game code — `skygame.dll`

The recompiled game is a **separate module inside the package**, not part of the exe
(`runtime/gamelib.c`, export `lasgame_tables`). The port stages it in step 8; this step produces it.

Upstream does this on the player's machine at runtime (`runtime/gamebuild.c` runs a bundled zig).
That is impossible in an AppContainer — no process creation, and no compiler on the console — so
patch `0002-gamebuild-no-on-console-compiler.patch` reports the situation instead of trying, and
the library has to be built here, on the PC, and shipped inside the package.

```
cd <upstream checkout>
make gen                     # tools/gen.py -> gamelib-src/ (generated C)
```

`make gen` needs the game files set up (see the repo README). It writes `gamelib-src/`, the
generated C for the guest, plus `GAMELIB_ID` — `5e5bafbb450f894b` for this revision
(`Makefile:114`, and the port's `CMakeLists.txt` compiles the runtime with the same value).

**The ABI is the part that matters.** The port's runtime is built with **clang-cl** (MSVC x64
ABI), because C++/CX and an AppContainer image require the MSVC toolchain, and the guest code needs
clang extensions MSVC does not have (`CMakeLists.txt:27`). Upstream's Windows build is *mingw* ABI
via `zig cc`, so a `skygame.dll` produced by `make` on Windows **cannot be loaded by this exe**.
Build it with the same compiler the runtime uses:

```
clang-cl /nologo /c /O2 /GS- /I runtime /I runtime/nvn /DGAMELIB_ID="5e5bafbb450f894b" <gamelib-src/*.c>
link /DLL /OUT:skygame.dll /DEF:gamelib.def *.obj
echo 5e5bafbb450f894b > skygame.dll.id
```

Two things will need attention the first time this is run, and both are visible from the source
rather than guessable: the export name (`GAMELIB_ENTRY`, `runtime/gamelib.h`) has to be in the
`.def`, and the entry returns the table struct by pointer, so the `.obj` set must match what
`gamelib-src/` generates for this game revision. If either is wrong, `gamelib.c` says so at
startup ("built for another game") rather than misbehaving later — the id file is checked before
the DLL is loaded.

## 2. SDL3 — the APP partition, no video

The version floor is **3.4.18** (`tools/build_deps_macos.sh`; `CMakeLists.txt` names it too).
It is not a preference: `device.c`'s screenshot path calls `SDL_SavePNG`, which does not exist in
SDL3 3.2 — the compiler says so.

```
git clone --branch release-3.4.18 --depth 1 https://github.com/libsdl-org/SDL sdl3
cmake -S sdl3 -B sdl3/build-uwp \
      -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0 \
      -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_SHARED=ON -DSDL_STATIC=OFF
cmake --build sdl3/build-uwp --config Release
```

What the runtime actually needs from SDL3 is the event queue, gamepads, sensors, audio and timing —
not a window and not a surface: the renderer draws into a shared image and D3D12 presents it
(`runtime/platform/uwp_present.cpp`). Keep the video subsystem if it builds; the port does not
call it.

## 3. FFmpeg 9.0.2, LGPL, static

Pinned at `9.0.2` with a deliberately minimal configuration
(`tools/build_deps_windows.sh`); the same configuration, cross-compiled for the APP partition.
The licence check matters: the script greps the pkg-config file for GPLv3 and fails the build,
because the game's movies are WebM and nothing here needs x264.

```
./configure --prefix=<abs>/ffmpeg-uwp --toolchain=msvc --target-os=win32 --arch=x86_64 \
    --enable-cross-compile --disable-static --enable-static --disable-shared \
    --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice \
    --disable-avfilter --disable-swresample --disable-everything \
    --enable-protocol=file --enable-demuxer=matroska,mov \
    --enable-decoder=vp9,vp8,vorbis,opus,h264,aac \
    --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
    --extra-cflags="-DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_CRT_APP"
```

`--enable-cross-compile` is needed because the configure script does not recognise the APP
partition as "Windows with a console"; it is a cross-compile to the same architecture.

## 4. shaderc, static

`runtime/nvn/vulkan/shaders.c` uses exactly one header, `<shaderc/shaderc.h>`, and four functions.
Build it as a static library for the APP partition (`-DSHADERC_SKIP_TESTS=ON
-DSHADERC_SKIP_EXAMPLES=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`); the port links it
statically, so nothing extra lands in the package.

## 5. The Vulkan driver — `vulkan_dzn.dll`

There is no native Vulkan driver on Xbox (`walbourn.github.io/directx-and-uwp-on-xbox-one/`), so
the port ships **Dozen**: Mesa's Vulkan-on-D3D12 driver, built as a UWP library. This is the
largest single step and the one that decides whether the port renders at all.

```
meson setup build-dzn --cross-file uwp-x64.txt \
      -Dvulkan-drivers=microsoft -Dgallium-drivers= -Dplatforms= \
      -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
      -Dllvm=disabled -Dshared-llvm=disabled \
      -Dbuildtype=release
```

Two constraints that come from the shipping Dozen port (`Cosmo/gow3_xbox`) rather than from Mesa:
clang-cl is mandatory (MSVC cannot build it), and the driver must be told to use the D3D12 backend
with **DXIL 1.7**. Link flags for the resulting DLL: `/APPCONTAINER /HIGHENTROPYVA:NO` — without
`/APPCONTAINER` the console refuses to load it into the sandbox, and without
`/HIGHENTROPYVA:NO` its address space assumptions do not hold.

## 6. `dxil.dll`

Dozen's D3D12 backend signs its shaders with DXIL, and the signing library comes from the DirectX
Shader Compiler release — `bin/x64/dxil.dll`, copied next to the exe. The port's CMake takes it as
`-DSKY_DXC_ROOT=<extracted DXC release>`; there is no Agility SDK on this platform
(`walbourn.github.io`, same page).

## 7. Configure and build the port

```
cmake -S xbox-uwp/port -B build-xbox-uwp -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
      -DSKY_ROOT=<upstream checkout with the patches applied> \
      -DSKY_DOZEN_DLL=<abs>/vulkan_dzn.dll \
      -DSKY_DXC_ROOT=<abs>/dxc \
      -DSKY_SDL3_BUILD=<abs>/sdl3/build-uwp \
      -DSKY_SDL3_SOURCE=<abs>/sdl3 \
      -DSKY_FFMPEG_ROOT=<abs>/ffmpeg-uwp \
      -DSKY_SHADERC_ROOT=<abs>/shaderc
```

`SKY_ROOT` must be the tree with `xbox-uwp/port/patches/*.patch` applied in order — the same seven
the compile workflow applies, which is what keeps this list from drifting:

```
for p in xbox-uwp/port/patches/*.patch; do git -C "$SKY_ROOT" apply "$p"; done
```

The project refuses to configure without all of the dependencies above, and it refuses to run
without clang-cl. Both are deliberate: the alternative is a link error forty minutes later that
does not say which of the four libraries is missing.

## 8. Package

```
pwsh xbox-uwp/packaging/New-DevCert.ps1
pwsh xbox-uwp/packaging/Package-Uwp.ps1 \
     -LayoutDir build-xbox-uwp/Release -PackageName SkyRecompUWPGame \
     -IdentityName SkyRecomp.Uwp.Game -DisplayName "Sky Recomp" -Executable skyrecomp_uwp.exe \
     -EntryPoint SkyRecomp.Uwp.App -GameDir <folder with skygame.dll and skygame.dll.id> \
     -CertPfx build-xbox-uwp/dev.pfx -CertPassword <password>
```

`-GameDir` is what makes it a game rather than a smoke test: it stages `skygame.dll` and
`skygame.dll.id` at the package root, where `LoadPackagedLibrary` looks
(`runtime/gamelib.c`, patch `0001`), and refuses a package whose id file is missing or malformed.
Without it the package builds, installs, starts, says the package has no game code in it, and
exits — and `INSTALL.md` inside the zip says the same thing.

**Getting the game onto the console.** A USB stick, laid out as
`<drive>:\<anything>\extracted\...`, is used directly as the data folder when it is writable:
the runtime scans the drives at startup, and the log says exactly what it found and why it chose
what it chose (`[usb] ...`, `[paths] data root: ...`). A read-only stick is reported and left
alone; with no stick at all the data folder is LocalState, which the Device Portal can browse.
The scan is also the measurement of what the `removableStorage` capability actually grants on a
console — the first run with a stick plugged in answers it in one line.

The artifact is `build-xbox-uwp/xbox-uwp.zip` + its SHA256 + `INSTALL.md`, `SHA256SUMS.txt` and the
VCLibs dependency appx. **Developer Mode only**: the certificate is self-signed, so the console has
to trust it, which Retail mode will not do.

## What is verified, today

`Compile UWP platform layer` runs on every push and compiles the port for the console
configuration:

* `uwp.c` and `uwp_present.cpp` — clang-cl, `-fsyntax-only`
* `uwp_entry.cpp` — MSVC `/Zw` (C++/CX cannot be parsed by clang-cl)
* `runtime/nvn/vulkan/device.c` — patched, compiled with `zig cc -target x86_64-windows-gnu`
* twelve runtime translation units — the consumers of the POSIX shims

Compiling *every* translation unit the CMake project lists is what found the hard links in
`romfs_build.c`, the missing SMAA include, and the SDL3 version floor. 51 of 54 compile; the three
that do not are waiting on FFmpeg and shaderc headers, which is why steps 3 and 4 exist.
