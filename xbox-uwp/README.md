# xbox-uwp — Xbox Developer Mode UWP packaging scaffold

This folder is the packaging half of an Xbox UWP build for
[SkyRecomp](https://github.com/Apolinario-coder/SkyRecomp). It builds, signs and packages
**a** UWP app today. It does **not** yet contain a ported game runtime — see
[`../BUILD_NOTES.md`](../BUILD_NOTES.md) §4 for why, and
[`../XBOX-UWP-PORT-PLAN.md`](../XBOX-UWP-PORT-PLAN.md) for the work that closes the gap.

Developer Mode sideloading only. Never retail mode.

```
xbox-uwp/
├── manifest/Package.appxmanifest.in   # Xbox manifest template (Windows.Xbox, x64, no full-trust)
├── packaging/New-DevCert.ps1          # self-signed code-signing cert (publisher must match manifest)
├── packaging/Package-Uwp.ps1          # stage -> makeappx -> signtool -> zip + SHA256SUMS
├── smoketest/                         # small D3D11 C++/CX UWP app: proves the pipeline
├── SkyRecompUwp/                      # the real runtime project (gated, currently #error)
└── assets/app-icon.png                # 1024x1024 icon from the upstream repo (docs/icon)
```

## What the smoke test is for

The smoke test exists so the pipeline is **provable before the port exists**. It is a
~200-line D3D11 C++/CX app that clears the screen to a moving colour for 15 seconds and
then exits (Back/Escape exits immediately). Deploying it proves four separate things that
are otherwise only discovered late:

1. a package signed with *your* certificate registers on *your* console,
2. UWP activation works (`CoreApplication::Run` + `IFrameworkView`, no XAML),
3. D3D11 device creation and CoreWindow presentation work on this console,
4. the tile/splash assets, display name and identity are correct in the shell.

If the smoke package deploys and shows colours, every packaging question is answered and
the only remaining work is the runtime port.

## Build it yourself (Windows 11 + VS 2022)

Requirements:

- Visual Studio 2022 with **Universal Windows Platform development** (C++ UWP tools, v143)
- **Windows 11 SDK 10.0.26100.0** (or another version; pass `/p:WindowsTargetPlatformVersion=...`)
- PowerShell 7 (`pwsh`) or Windows PowerShell 5.1

```powershell
# 1. build the app
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
& $msbuild xbox-uwp\smoketest\SkyRecompUwpSmoke.vcxproj /p:Configuration=Release /p:Platform=x64

# 2. create a signing certificate (keep the .pfx and its password; they are yours)
pwsh -File xbox-uwp\packaging\New-DevCert.ps1 -OutDir build-xbox-uwp -Password 'pick-something'

# 3. package
pwsh -File xbox-uwp\packaging\Package-Uwp.ps1 `
    -LayoutDir    'xbox-uwp\smoketest\x64\Release' `
    -PackageName  'SkyRecompUwpSmoke' `
    -IdentityName 'SkyRecomp.UwpSmoke' `
    -DisplayName  'Sky Recomp UWP Smoke' `
    -Executable   'SkyRecompUwpSmoke.exe' `
    -EntryPoint   'SkyRecompUwpSmoke.App' `
    -Version      '1.0.0.0' `
    -IconSource   'xbox-uwp\assets\app-icon.png' `
    -ArtifactName 'xbox-uwp-smoketest' `
    -CertPfx      'build-xbox-uwp\SkyRecompUwp.pfx' `
    -CertPassword 'pick-something'
```

Output, all in `build-xbox-uwp/`:

| File | What it is |
|---|---|
| `xbox-uwp-smoketest.zip` | the deliverable: `.msix` + `.cer` + `INSTALL.md` + `SHA256SUMS.txt` |
| `SkyRecompUwpSmoke_1.0.0.0_x64.msix` | the signed package itself |
| `SkyRecompUwpSmoke.cer` | public certificate — trust this on the console |
| `_stage/` | the package layout, for inspecting what actually went in |

## Build it in CI

`.github/workflows/build-xbox-uwp.yml` runs on `windows-2022` (VS 2022 + SDK 10.0.26100.0)
and uploads `xbox-uwp-smoketest` as an artifact. Trigger it with **Actions → Build Xbox UWP
(Dev Mode) → Run workflow**, or push any change under `xbox-uwp/`.

The runtime job (`xbox-uwp.zip`) is disabled unless the repository variable
`ENABLE_UWP_RUNTIME_BUILD` is `true`, and its first step fails fast if `PortBlocked.cpp` is
still present — so enabling the variable can never silently produce a broken package.

## Deploy to the console

1. **Console**: install *Xbox Dev Mode* from the Store, activate it at
   `https://partner.microsoft.com/xboxconfig`, restart into Developer Mode.
2. **Console**: *Dev Home → Settings → Remote Access* → enable, set credentials, note the URL
   (`https://<console-ip>:11443`).
3. **PC**: trust the certificate on the console — Device Portal → certificates, upload
   `SkyRecompUwpSmoke.cer` (the console rejects untrusted signers with `0x800B0109`).
4. **PC**: Device Portal → *Apps* → add/register package. If the zip has a `dependencies\`
   folder, install `Microsoft.VCLibs.x64.14.00.appx` **first** — the app links the DLL CRT
   (`/MD` is mandatory with C++/CX, `/MT` fails with `D8016`), so the VC++ runtime must be
   present before the app. Then select the `.msix`.
5. **Console**: *Dev Home → app → View details → App type = Game* (App mode has a much
   smaller memory budget).
6. Launch from *My games & apps* in the sandbox. Expect a moving colour for 15 s.

## Honest status of these scripts

| Piece | Status |
|---|---|
| `Package.appxmanifest.in` | reviewed against the AppX schema and Microsoft's ATG Xbox samples |
| `Package-Uwp.ps1` / `New-DevCert.ps1` | **executed for real in CI and green**: makeappx, signtool sign and verify /pa all succeed |
| `smoketest/*.vcxproj`, `App.cpp` | **compiles** with MSVC v143 on `windows-2022`; packaged, signed and verified. Four failures on the way there are tabulated below |
| `SkyRecompUwp/*` | intentionally non-building scaffold (`PortBlocked.cpp`) |

### Verified build (CI)

The workflow is green on `windows-2022`. Four failures on the way there, and what each taught:

| Attempt | Failure | Fix |
|---|---|---|
| 1 | `D8016: '/MT' and '/ZW' are incompatible` | C++/CX requires the DLL CRT → `/MD`; packager now ships `dependencies\Microsoft.VCLibs.x64.14.00.appx` (install before the app) |
| 1 | `/std:c++17` also incompatible with `/ZW` | removed `<LanguageStandard>` |
| 2 | `C2065 'IDXGISwapChain1'/'IDXGIFactory2' undeclared`, cascading `ComPtr` errors | `<dxgi.h>` only declares up to `IDXGISwapChain`; switched to `<dxgi1_6.h>` + `<d3d11_3.h>`, with `<wrl.h>` guarded by `warning(disable:4467)` as the ATG samples do |
| 2 | `C2679` assigning `Rect` to `Size` | `CoreWindow::Bounds` is a `Rect` |
| 3 | `signtool verify`: "root certificate which is not trusted" | self-signed cert on a clean machine — trust it first (`certutil -addstore -f`) |
| 4 | trust step **hung for 15 minutes** | `X509Store.Add` to `Root` raises a blocking dialog when headless; `certutil -addstore -f` is non-interactive |

Result, from run [#41037609285](https://github.com/youdedend/skyrecompuwp/actions/runs/41037609285):

| | |
|---|---|
| Artifact | `xbox-uwp-smoketest.zip` |
| Size | 1,083,048 bytes |
| SHA256 | `bb64f58b158ba19e6b8aad914b3f618dca6b6f93f90b9d40e09f9fc3560ef2eb` |
| Contents | signed `SkyRecompUwpSmoke_1.0.0.0_x64.msix`, `SkyRecompUwpSmoke.cer`, `dependencies\Microsoft.VCLibs.x64.14.00.appx`, `INSTALL.md`, `SHA256SUMS.txt` |
| Download | Actions → *Build Xbox UWP (Dev Mode)* → the run → Artifacts |

That is a real, signed, deployable UWP package — of the smoke test, not of the game.

## Licence / provenance

`assets/app-icon.png` and `assets/app-icon.svg` are copied unmodified from the upstream
SkyRecomp repository (`docs/icon/`), MIT-licensed together with the rest of that project.
The smoke test and packaging scripts here are new.
