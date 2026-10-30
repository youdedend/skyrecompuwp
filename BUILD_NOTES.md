# BUILD_NOTES — Xbox UWP package attempt (SkyRecomp)

**Status: NO `xbox-uwp.zip` WAS PRODUCED. This is an audit + blocker report, not a successful build.**

Reason in one sentence: the repository has **no Xbox/UWP target at all** — no AppxManifest, no Visual Studio/MSIX project, no D3D12 renderer, no Xbox GDK usage, no UWP build instructions, and no prebuilt `xbox-uwp.zip` in Releases or CI artifacts. Its only documented Windows build is a **Win32 desktop .exe built with MSYS2 UCRT64 clang + Vulkan**, which cannot run on an Xbox (consoles have no Vulkan and UWP apps cannot use the desktop Win32 API surface).

Reading this file top-to-bottom gives you: what the repo actually says, the exact evidence, why an Xbox build is impossible without porting work, what that port costs, and the sideloading procedure to use once a real package exists.

---

## 1. Repositories inspected

| Item | Value |
|---|---|
| Source repo audited | `https://github.com/Apolinario-coder/SkyRecomp` |
| Commit audited (HEAD of `main`) | `f005a619b1732376ac355243cc5b30e2ccc89ca9` |
| Commit date / subject | `2026-10-29T19:43:29-03:00` — "Merge pull request #77 from BlAnK182C-bak/feat/gyro" |
| Version in-tree | `#define LARECOMP_VERSION "v2.0.2"` (`runtime/ui/launcher.mm:89`) |
| Latest release | `v2.0.2` (published 2026-10-26T02:31:48Z) |
| Session repo (this branch) | `youdedend/skyrecompuwp` @ `ab988e1` — contains **only** a 1-line `README.md`; no source |
| Branches in upstream repo | `main` only |

Because the session repo is an empty stub, all work below was done against a clone of the upstream repo in `/home/user/.scratch-skyrecomp` (not part of the git tree).

---

## 2. What the repo says about Xbox UWP support

**Nothing. There is zero Xbox/UWP support, documented or in code.**

Full-text search over the repo excluding `third_party/` for `xbox|uwp|xgame|GDK|devmode|AppxManifest|msix|sideload` returns only:

- `README.md:27` — "Seamless support for modern gamepads (**Xbox**, DualSense…)" → a *controller* name, not a platform.
- `runtime/hle/bindings.c`, `runtime/hle/input.c`, `runtime/nvn/texpatch.c` — Xbox **button-prompt artwork** (A/B/X/Y glyphs) and `SDL_GAMEPAD_TYPE_XBOXONE`. Nothing platform-related.
- `third_party/imgui/docs/...` — upstream ImGui release notes mentioning UWP once. Irrelevant.

Structural evidence:

| Check | Result |
|---|---|
| `Package.appxmanifest` / `*.appxmanifest` / `*.wapproj` / `*.msix` anywhere | **none** |
| `.sln` / `.vcxproj` for the app | **none** (only vendored ImGui example projects) |
| Root `CMakeLists.txt` / `CMakePresets.json` / `vcpkg.json` | **none** — the build is a hand-written `Makefile` |
| `docs/` build guides | `BUILD-linux.md`, `BUILD-macos.md` only — **no `BUILD-windows.md`**, even though `Makefile:4` points readers at it |
| `Makefile` platform switch | `windows` / `linux` / `macos` only (`Makefile:5-13`) |
| CI jobs (`build-releases.yml`) | `build-linux`, `build-linux-appimage`, `build-macos` — **no Windows job, no UWP job** |
| CI `source-checks.yml` | Python hygiene tests only |
| Graphics backends present | Vulkan (`runtime/nvn/vulkan/`, Windows+Linux) and Metal (`runtime/nvn/*.m`, macOS) — **no D3D11/D3D12 backend**, `grep -ri d3d12\|dxgi\|d3dcompiler runtime/ tools/` = 0 hits |

So: the phrase "the repo's official Xbox UWP build instructions" has no referent. There are none to follow, and none to deviate from.

### What the repo *does* document (the only real build paths)

`README.md → Compiling from Source`:

```bash
# Windows (MSYS2 UCRT64 shell, clang + lld + make)
make build/skyrecomp.exe

# Linux (docs/BUILD-linux.md)
make build/skyrecomp
make appimage

# macOS (docs/BUILD-macos.md)
make deps && make app && make dmg
```

The Windows link line (`Makefile:96-99`) is the definition of "not UWP":

```make
LDFLAGS := -fuse-ld=lld -Wl,--stack,16777216 ... -lavformat -lavcodec -lavutil -lswscale -lSDL3 \
           -lvulkan-1 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
           $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)
```

`-Wl,--subsystem,windows` = a desktop Win32 GUI subsystem binary, produced by MinGW-flavoured clang/LLD, linking `vulkan-1`. That is a desktop executable, full stop.

---

## 3. Prebuilt package check (Release + CI artifacts)

**No prebuilt `xbox-uwp.zip` exists. Nothing to download, so nothing was verified beyond metadata.**

Releases (all 9, via `GET /repos/Apolinario-coder/SkyRecomp/releases`):

| Tag | Published | Assets |
|---|---|---|
| v2.0.2 | 2026-10-26 | AppImage, Linux.tar.gz, Linux.zip, macOS.dmg, macOS.zip, **Windows.zip (1,881,203,900 B)** |
| v2.0.1 | 2026-10-25 | same 6 |
| v2.0.0 | 2026-10-25 | same 6 |
| v1.0.6 | 2026-10-22 | same 6 |
| v1.0.5 | 2026-10-20 | 5 assets |
| v1.0.4 | 2026-10-19 | 5 assets |
| v1.0.3 | 2026-10-18 | 5 assets |
| v1.0.2 | 2026-10-17 | Windows.zip only |
| v1.0.1-beta | 2026-10-17 | Windows.zip only |

- No `.appx`, `.msix`, `.appxbundle`, `.msixbundle`, `AppxManifest.xml`, or any file named `xbox*` in any release.
- GitHub Actions artifacts: `GET /repos/.../actions/artifacts` → **`total_count: 0`** (workflows only upload to Releases; no artifact retention).
- The `Windows.zip` asset is the desktop build: release notes say "Extract the ZIP archive completely before running **skyrecomp.exe**", and README ships `skyrecomp.exe` as the Windows entry point. It is not a UWP/MSIX package.
- Its SHA256 is published by GitHub as asset metadata (useful only as a desktop-build reference):
  `5d8dd9158d44b005759a0c6d061febcc148ff29b820e6eb85a4ac5ead3898e0f`
  (`https://github.com/Apolinario-coder/SkyRecomp/releases/download/v2.0.2/SkyRecomp-v2.0.2-Windows.zip`)
  I did **not** download it: binary downloads redirect to `objects.githubusercontent.com`, which this sandbox cannot reach (TLS connection closed). Only API metadata was verified. It is irrelevant to the Xbox target anyway.

Fork sweep (`GET /repos/.../forks`, 30+ forks, plus repo search for "skyrecomp"/"lasrecomp"): no fork advertises an Xbox/UWP port; upstream `peiikai/LASRecomp` is macOS/Windows/Linux only.

---

## 4. Can it be built from source as UWP here? No — and not just because of this sandbox

### 4.1 This environment cannot do it at all

| Requirement | Present? |
|---|---|
| Windows host | ✗ (Debian 12 x86_64) |
| MSVC + C++ UWP tools (`/ZW`, `/store` libs) | ✗ |
| Windows 11 SDK (any version, incl. 10.0.26100.0) | ✗ |
| `makeappx.exe`, `makepri.exe`, `signtool.exe` | ✗ (Windows-only, closed-source; no supported cross-platform equivalent) |
| Xbox GDK | ✗ |
| MSYS2 UCRT64 clang/lld (the repo's actual Windows toolchain) | ✗ |
| clang / cmake / ninja / mingw-w64 / wine | ✗ (only gcc, make, python3, git, zip) |

Even with all of that installed, it would still fail: mingw-w64 cannot produce a Store/UWP app package. MSVC's UWP configuration relies on the separate `store` import libraries and `WINAPI_FAMILY=WINAPI_FAMILY_APP`; mingw-w64 only offers `winstorecompat`/`windowsappcompat` stubs, which is enough for libraries, not for building the app package.

### 4.2 The blockers are in the source, not in the toolchain

These are ordered by how fatal they are. Each is measured from the code, not guessed.

**B1 — No Vulkan on Xbox. The renderer must be rewritten in D3D12.**
`runtime/nvn/vulkan/` is 6,452 lines (`exec.c` alone 3,900) implementing the Maxwell/NVN → Vulkan 1.3 pipeline, with VMA, shaderc (runtime GLSL→SPIR-V), dynamic rendering and FSR/SMAA passes. Xbox exposes **Direct3D only**; UWP on Xbox supports D3D11 FL10.0/11.0 and D3D12 FL11.0/Shader Model 6.4, with no Agility SDK on console. There is no Vulkan ICD and no viable Vulkan-on-D3D12 shim for a 1.3-class renderer. This is a ground-up graphics backend, not a flag flip.

**B2 — The AppContainer kills the runtime's process model.**
`runtime/platform/windows.c` uses `CreateProcessW` 4× (child process launch, self re-exec, console attach) and `CreateJobObjectW`; `runtime/gamebuild.c` shells out to a bundled **Zig 0.17.0** compiler (`zig cc`) at setup time to build `skygame.dll` **on the player's machine**, and `launcher.mm:277` tells the user to keep `skygen` and `zig` next to the exe. Spawning arbitrary child processes and loading a freshly compiled MinGW DLL are both blocked in a UWP AppContainer. The whole "embedded setup GUI + runtime compilation" design has to be replaced by a pre-baked game library produced off-console.

**B3 — 5,120 MiB memory ceiling vs. a 12.5 GB guest address window.**
`runtime/rt.h:15`: the guest lives in a 12.5 GB window at `0x70_0000_0000` claimed at startup. Xbox Dev Mode UWP apps get a **hard 5,120 MiB unified RAM+VRAM limit** in Game resource mode (much less in App mode), enforced by the Resource Manager; going over gets you `E_OUTOFMEMORY` and then suspension. Skyward Sword HD alone occupies ~3.2 GiB of guest memory, and this runtime keeps host-side copies of guest textures — before the 4K texture pack. Reserving 12.5 GB of *virtual* space is survivable; *committing* enough of it is not, without a serious memory diet that does not exist today.

**B4 — File-system model.**
The launcher is a folder picker + free-form writer (game files folder, `mods/`, `save/`, texture packs, `skyrecomp.log`, shell-open of folders via `ShellExecute`/`xdg-open`/`open`). UWP requires `KnownFolders`/`StorageFile` brokering or `broadFileSystemAccess` (not available on Xbox). Roughly 15 GB of user-supplied assets have to become package content or a brokered library folder.

**B5 — SDL3 under UWP is unproven for this version.**
The repo pins SDL3 `release-3.4.18`. SDL2 had a WinRT/UWP backend; SDL3's status for AppContainer/Xbox has to be verified per release (there is no in-repo evidence either way, and the repo only builds SDL3 for Win32/MSYS2, Linux and macOS). Audio also moves from WASAPI desktop paths to the UWP-legal surface.

**B6 — Undocumented Windows build.**
`Makefile:4` points at `docs/BUILD-windows.md`, which does not exist. The Windows build is only described by one README line plus `tools/setup_windows.ps1` / `tools/build_deps_windows.sh`. There is no reproducible, versioned Windows recipe to extend into a UWP variant.

Three recent, comparable projects confirm the shape of the work: `JulianDr14/jadyx` / `JulianDr14/eden-xbox` / `PrismFox67/Eden-UWP` (Switch emulators ported to UWP for Xbox Series X|S Dev Mode) all had to write **their own D3D12 backend**, translate SPIR-V→DXIL via Mesa + `dxil.dll`, restructure memory around the 5,120 MiB budget, and ship via MSIX with a self-signed cert. That is the minimum viable path for any Vulkan/SDL desktop app on Xbox.

---

## 5. Required build-notes fields

**Repo commit hash**
- Audited source: `Apolinario-coder/SkyRecomp` @ `f005a619b1732376ac355243cc5b30e2ccc89ca9` (`main`, 2026-10-29)
- Session repo/branch: `youdedend/skyrecompuwp` @ `ab988e1` + this branch `arena/f3ff2dab-skyrecompuwp` (docs only)

**Tool versions used**
- Audit environment: Debian GNU/Linux 12 (bookworm), x86_64; `git`, `curl`, `gh` (GitHub CLI, authenticated), `python3` 3.11, `make`, `gcc`, `zip`. No Windows toolchain of any kind.
- Toolchain the repo *requires* for its existing (non-UWP) outputs: MSYS2 UCRT64 `clang`/`lld`/`make`/`windres` + NASM on Windows; `clang`/`lld`/GNU `make` on Linux/macOS; Python 3.13 in CI (3.x locally for `tools/`); PyInstaller 6.22.3 for the setup helper.
- Toolchain a Xbox UWP product *would* require: Visual Studio 2022 with "Universal Windows Platform development" + "Game development with C++", Windows 11 SDK (10.0.26100.0 or current), MSIX packaging tools (`makeappx`/`makepri`/`signtool`), and for console-specific work the Xbox GDK (only for the GDK path; classic dev-mode UWP/MSIX does not need it).

**Exact build commands**
- Commands actually executed: audit only (`git ls-remote`, `git clone --filter=blob:none`, `git grep`, `gh api .../releases`, `gh api .../actions/artifacts`, `gh search ...`). **No build was started** — per your rule 5, I stop for approval, and per §4 there is no UWP build to approve.
- Documented repo commands (desktop, **not** UWP, listed for completeness):
  ```bash
  # Windows desktop (MSYS2 UCRT64)  ->  build/skyrecomp.exe (Win32 GUI, Vulkan)
  make build/skyrecomp.exe
  # Linux
  make build/skyrecomp && make appimage
  # macOS
  make deps && make app && make dmg
  ```
- There is **no** documented or inferable command that produces an Xbox UWP/MSIX package. Nothing was invented.

**Dependency versions (as pinned/in-tree)**
- SDL3 `release-3.4.18` (from `build-releases.yml`)
- FFmpeg 9.0.2, LGPL-only, pinned sha256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e` (`tools/build_deps_windows.sh`)
- Zig 0.17.0 (pinned per-platform SHA256 in `tools/fetch_zig.sh`), used at runtime to compile the game library
- shaderc (system/`-lshaderc_shared`), Vulkan loader `vulkan-1` + Vulkan 1.3 driver at runtime (desktop)
- capstone 5.0.9, unicorn 2.1.4, lz4 4.4.5, PyInstaller 6.22.3 (setup helper build)
- Vendored: Dear ImGui, SMAA, xxhash, ReShade payload (`third_party/`)
- Game data: user-supplied Skyward Sword HD **v1.0.0** (`romfs`/`exefs`); never bundled

**Artifact path and checksum**
- `xbox-uwp.zip`: **not produced**. No path, no checksum — anything I emitted would be a non-runnable file, and shipping one would be worse than shipping nothing.
- Reference (desktop, upstream, does **not** run on Xbox): `SkyRecomp-v2.0.2-Windows.zip`, 1,881,203,900 B, SHA256 `5d8dd9158d44b005759a0c6d061febcc148ff29b820e6eb85a4ac5ead3898e0f`.

**Known issues / gaps**
1. No UWP target, manifest, project files or instructions upstream (see §2).
2. No D3D12/D3D11 renderer — Vulkan and Metal only (B1).
3. Runtime child-process model (`CreateProcessW`, job objects) and on-device Zig compilation are AppContainer-illegal (B2).
4. Guest memory model exceeds the 5,120 MiB Xbox UWP Game-mode ceiling (B3).
5. Unbrokered free-form file system access throughout the launcher (B4).
6. SDL3 3.4.18 Xbox/UWP support unverified; no in-repo UWP SDL build (B5).
7. Missing `docs/BUILD-windows.md` that `Makefile` references (B6).
8. UWP packaging tools (`makeappx`, `signtool`) are Windows-only; no supported cross-build path exists, so this can only be done on a Windows 11 host.
9. `-march=x86-64-v3` (AVX2+FMA) for the lifted code: fine on Series X|S (Zen 2), **not** on Xbox One (Jaguar, AVX1 only) — an Xbox One target would need `GENARCH` changes.

---

## 6. Developer Mode sideloading (for when a real `xbox-uwp.zip` exists)

Developer Mode only — this never works in Retail mode, and it needs no Store account, publisher approval, or GDK licence.

**On the console (one time)**
1. Install **Xbox Dev Mode** from the Store on the console, note the activation code, redeem it at `https://partner.microsoft.com/xboxconfig` with a Microsoft account, wait for activation, then **restart into Developer Mode**.
2. In **Dev Home → Settings → Remote Access**, enable the Xbox Device Portal and set a username/password. Note the URL it shows, normally `https://<console-ip>:11443`.
3. In **Dev Home → Settings**, make sure the sandbox has enough space allocated (the app + assets need it).

**On your PC (same LAN)**
4. Open `https://<console-ip>:11443`, accept the self-signed certificate warning, sign in with the credentials from step 2.
5. Trust the signing certificate: upload the package's `.cer` via the portal (or install it into the sandbox's trusted store) before deploying. A package whose signer is not trusted is rejected with `0x800B0109`.
6. **Apps → Add / Upload**, choose the `.msix`/`.appxbundle` (plus any dependency packages such as `Microsoft.VCLibs.x64.14.00.appx` if the build requests them), and deploy. Failures to check in order: wrong architecture (Xbox is **x64**), untrusted certificate, missing dependency, capability not declared in the manifest.
7. After install, open the app's details in Dev Home and set **App type → Game** — that is what unlocks the full 5,120 MiB memory allocation instead of the much smaller App-mode budget.
8. Launch from Dev Home's **My games & apps** in the Developer Mode sandbox. Game assets must be placed where the package can read them (package content or a brokered library folder) — copy them with the portal's **File Explorer** or a USB drive.
9. Switch back to Retail Mode any time via **Dev Home → Leave Dev Mode**.

Verification commands you can run before uploading (Windows PowerShell, Windows SDK installed):

```powershell
Get-FileHash .\xbox-uwp.zip -Algorithm SHA256
Expand-Archive .\xbox-uwp.zip -DestinationPath .\pkg
Get-AppxPackageManifest .\pkg\AppxManifest.xml | Select-Object -ExpandProperty Package
signtool verify /pa /all /v .\pkg\*.msix
```

---

## 7. What would have to happen to actually produce `xbox-uwp.zip`

See `XBOX-UWP-PORT-PLAN.md` in this repo for the file-by-file work breakdown. Short version: this is a **port project measured in weeks**, not a build. The critical path is a D3D12 backend for `runtime/nvn/` (replacing 6.4k lines of Vulkan), a UWP platform layer replacing `runtime/platform/windows.c`, a pre-baked (no runtime compiler) game-library flow, a memory budget that fits 5,120 MiB, and MSIX packaging + signing — all on a Windows 11 host with VS 2022 and the Windows SDK.

## 8. Decisions I need from you

1. Do you want me to **scaffold the UWP/MSIX project** (manifest, capability set, packaging script, CI workflow) so the packaging side is ready and testable the moment a D3D12 backend exists? That is real, reviewable work I can do now.
2. Do you have a **Windows 11 build host** (or a GitHub Actions Windows runner) you want me to target? Every real step needs one; this Linux sandbox cannot build or package UWP.
3. Is a **fork/branch with Xbox work** the intended source (e.g. someone else's UWP branch)? Give me the URL and I will audit and build that instead.
4. Or was `xbox-uwp.zip` expected to be a **desktop→Xbox wrapper** (it cannot work: no full-trust/desktop bridge exists in Xbox Dev Mode), in which case: no.
