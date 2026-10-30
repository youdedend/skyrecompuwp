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
4. **PC**: Device Portal → *Apps* → add/register package → choose the `.msix`.
   Architecture is x64; if it asks for a dependency (VCLibs), this project uses the static
   CRT deliberately so it should not.
5. **Console**: *Dev Home → app → View details → App type = Game* (App mode has a much
   smaller memory budget).
6. Launch from *My games & apps* in the sandbox. Expect a moving colour for 15 s.

## Honest status of these scripts

| Piece | Status |
|---|---|
| `Package.appxmanifest.in` | reviewed against the AppX schema and Microsoft's ATG Xbox samples |
| `Package-Uwp.ps1` / `New-DevCert.ps1` | structurally validated here (here-strings, brace/paren balance); **not executed in this Linux sandbox** — first real run is the CI job |
| `smoketest/*.vcxproj`, `App.cpp` | modelled on Microsoft's Xbox ATG UWP samples; compiled only on CI |
| `SkyRecompUwp/*` | intentionally non-building scaffold (`PortBlocked.cpp`) |

If CI fails, the failure will be in one of three places, in this order of likelihood:
the UWP C++ toolset version on the image (the workflow retries `v142` automatically), an MSBuild
property the SDK moved, or a WinRT type used in `App.cpp` that needs a namespace include.
Paste the log and it can be fixed directly — none of those are architectural.

## Licence / provenance

`assets/app-icon.png` and `assets/app-icon.svg` are copied unmodified from the upstream
SkyRecomp repository (`docs/icon/`), MIT-licensed together with the rest of that project.
The smoke test and packaging scripts here are new.
