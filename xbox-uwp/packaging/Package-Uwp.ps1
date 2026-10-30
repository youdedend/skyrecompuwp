<#
.SYNOPSIS
  Builds a signed, Xbox-ready UWP/MSIX package and wraps it into a zip with checksums.

.DESCRIPTION
  This is the single packaging entry point used by CI and by anyone building locally.
  It does exactly five things and nothing magical:

    1. stages the built app layout into a clean package directory
    2. renders AppxManifest.xml from xbox-uwp/manifest/Package.appxmanifest.in
    3. generates the tile/splash PNG assets (or scales a real icon you pass in)
    4. runs makeappx.exe pack, then signtool.exe sign
    5. writes INSTALL.md + SHA256SUMS.txt and zips everything to <ArtifactName>.zip

  Signing is mandatory for Xbox: an unsigned package is rejected by the deployment
  stack. Use -SkipSign only for local layout inspection.

.PARAMETER LayoutDir
  Directory containing the built .exe (e.g. the MSBuild output folder).

.PARAMETER OutputDir
  Where the zip, the .msix and the hashes are written (default: build-xbox-uwp).

.EXAMPLE
  pwsh -File xbox-uwp/packaging/Package-Uwp.ps1 `
       -LayoutDir xbox-uwp/smoketest/x64/Release `
       -PackageName SkyRecompUwpSmoke `
       -IdentityName SkyRecomp.UwpSmoke `
       -DisplayName "Sky Recomp UWP Smoke Test" `
       -Executable SkyRecompUwpSmoke.exe `
       -EntryPoint SkyRecompUwpSmoke.App `
       -Version 1.0.0.0 `
       -ArtifactName xbox-uwp-smoketest `
       -CertPfx build-xbox-uwp/SkyRecompUwp.pfx -CertPassword skyrecomp
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$LayoutDir,
    [Parameter(Mandatory = $true)][string]$PackageName,
    [Parameter(Mandatory = $true)][string]$IdentityName,
    [Parameter(Mandatory = $true)][string]$DisplayName,
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$EntryPoint,

    [string]$Description  = "Xbox UWP package",
    [string]$ShortName    = "",
    [string]$Version      = "1.0.0.0",
    [string]$Publisher    = "CN=SkyRecomp Dev",
    [string]$PublisherDisplay = "SkyRecomp",
    [string]$OutputDir    = "build-xbox-uwp",
    [string]$ArtifactName = "xbox-uwp",
    [string]$ManifestTemplate = "",
    [string]$IconSource   = "",
    [string]$IconLabel    = "",
    [string]$CertPfx      = "",
    [string]$CertPassword = "",
    # The folder where `make gen && make` on a PC left skygame.dll and skygame.dll.id. Both are
    # required: gamelib.c reads the .id without loading the library, to decide whether the game code
    # in the package was built for this runtime. Without this switch the package is a smoke test -
    # it starts, says "this package has no game code in it", and exits.
    [string]$GameDir      = "",
    # The app links the DLL CRT (/MD is mandatory with C++/CX), so the Microsoft.VCLibs
    # framework package has to be installed on the console before the app. Leave empty to
    # auto-locate it in the Windows SDK ExtensionSDKs; the packager ships it in the zip.
    [string]$VclibsPath   = "",
    [switch]$SkipSign
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Section([string]$text) {
    Write-Host ""
    Write-Host "== $text" -ForegroundColor Cyan
}

# ---------------------------------------------------------------- tool discovery

function Find-SdkTool([string]$name) {
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (-not (Test-Path $kits)) { throw "Windows SDK not found at $kits" }
    $tool = Get-ChildItem -Path $kits -Filter $name -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } |
        Sort-Object { $_.FullName } -Descending |
        Select-Object -First 1
    if (-not $tool) { throw "$name not found in $kits. Install the Windows 11 SDK (10.0.26100.0 or newer)." }
    return $tool.FullName
}

function Find-Vclibs {
    if ($VclibsPath -ne "") {
        if (-not (Test-Path $VclibsPath)) { throw "VclibsPath does not exist: $VclibsPath" }
        return (Resolve-Path $VclibsPath).Path
    }
    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft SDKs\Windows\v10.0\ExtensionSDKs\Microsoft.VCLibs")
    )
    foreach ($root in $roots) {
        if (Test-Path $root) {
            $hit = Get-ChildItem -Path $root -Recurse -Filter "Microsoft.VCLibs.x64.14.00.appx" -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

Write-Section "Resolving toolchain"
if (-not (Test-Path $LayoutDir)) { throw "LayoutDir not found: $LayoutDir" }
$LayoutDir = (Resolve-Path $LayoutDir).Path
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$makeappx = Find-SdkTool "makeappx.exe"
$signtool = if ($SkipSign) { $null } else { Find-SdkTool "signtool.exe" }
Write-Host "makeappx : $makeappx"
if ($signtool) { Write-Host "signtool : $signtool" }

if ($ManifestTemplate -eq "") {
    $ManifestTemplate = Join-Path $PSScriptRoot "..\manifest\Package.appxmanifest.in"
}
$ManifestTemplate = (Resolve-Path $ManifestTemplate).Path

# ---------------------------------------------------------------- stage payload

Write-Section "Staging package layout"
$stage = Join-Path $OutputDir "_stage\$PackageName"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$exclude = @('*.pdb', '*.ilk', '*.exp', '*.lib', '*.obj', '*.iobj', '*.ipdb', '*.tlog')
Get-ChildItem -Path $LayoutDir -Recurse -File | Where-Object {
    $rel = $_.FullName.Substring($LayoutDir.Length).TrimStart('\')
    $skip = $false
    foreach ($p in $exclude) { if ($_.Name -like $p) { $skip = $true } }
    -not $skip
} | ForEach-Object {
    $rel = $_.FullName.Substring($LayoutDir.Length).TrimStart('\')
    $dest = Join-Path $stage $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
}

$exePath = Join-Path $stage $Executable
if (-not (Test-Path $exePath)) { throw "Executable '$Executable' is not in the layout ($LayoutDir). Contents: $(Get-ChildItem $LayoutDir -Name | Select-Object -First 20)" }
Write-Host "payload: $LayoutDir -> $stage"

# ---------------------------------------------------------------- game code
# The recompiled game is a second module inside the package: an AppContainer loads only DLLs that
# are part of it, and only by a bare name from the package root (runtime/gamelib.c). So it goes in
# here, at the top level, next to the .exe - not in a subfolder.
Write-Section "Game code"
$hasGameCode = $false
$gameId = ""
if ($GameDir) {
    if (-not (Test-Path $GameDir)) { throw "GameDir does not exist: $GameDir" }
    $libPath = Join-Path $GameDir "skygame.dll"
    $idPath  = "$libPath.id"
    if (-not (Test-Path $libPath)) {
        throw "skygame.dll is not in GameDir ($GameDir). Build it there first: run "make gen" then "make" in a source checkout, with the game files set up."
    }
    if (-not (Test-Path $idPath)) {
        throw "skygame.dll.id is not in GameDir ($GameDir). gamelib.c reads it (without loading the DLL) to check the game code matches this runtime; without it every load fails with "built for another game"."
    }
    $gameId = (Get-Content -LiteralPath $idPath -Raw).Trim()
    if ($gameId -notmatch '^[0-9A-Za-z._-]+$') {
        throw "skygame.dll.id does not contain a single id token: '$gameId'"
    }
    Copy-Item -LiteralPath $libPath -Destination (Join-Path $stage "skygame.dll") -Force
    Copy-Item -LiteralPath $idPath  -Destination (Join-Path $stage "skygame.dll.id") -Force
    $libMb = [math]::Round((Get-Item $libPath).Length / 1MB, 1)
    Write-Host "skygame.dll    : $libMb MB, id '$gameId'"
    $hasGameCode = $true
} else {
    Write-Warning "No -GameDir: this package contains NO game code. It will start, report that the package has no game code in it, and exit. Pass -GameDir <folder with skygame.dll + skygame.dll.id> to make an installable game."
}

# ---------------------------------------------------------------- manifest

Write-Section "Rendering AppxManifest.xml"
if ($ShortName -eq "") {
    $ShortName = if ($DisplayName.Length -le 20) { $DisplayName } else { $DisplayName.Substring(0, 20) }
}
$tokens = @{
    '{{IDENTITY_NAME}}'     = $IdentityName
    '{{PUBLISHER}}'         = $Publisher
    '{{VERSION}}'           = $Version
    '{{DISPLAY_NAME}}'      = $DisplayName
    '{{SHORT_NAME}}'        = $ShortName
    '{{PUBLISHER_DISPLAY}}' = $PublisherDisplay
    '{{DESCRIPTION}}'       = $Description
    '{{EXECUTABLE}}'        = $Executable
    '{{ENTRYPOINT}}'        = $EntryPoint
}
$manifest = Get-Content -Raw -LiteralPath $ManifestTemplate
foreach ($k in $tokens.Keys) { $manifest = $manifest.Replace($k, $tokens[$k]) }
if ($manifest -match '\{\{[A-Z_]+\}\}') {
    throw "Unreplaced manifest tokens remain: $($Matches[0])"
}
# validate before handing it to makeappx: a malformed manifest produces useless SDK errors
try { [xml]$manifest | Out-Null } catch { throw "Generated manifest is not valid XML: $($_.Exception.Message)" }
$manifestPath = Join-Path $stage "AppxManifest.xml"
Set-Content -LiteralPath $manifestPath -Value $manifest -Encoding UTF8
Write-Host "manifest: $manifestPath"

# ---------------------------------------------------------------- assets

Write-Section "Generating package assets"
Add-Type -AssemblyName System.Drawing

$assetDir = Join-Path $stage "Assets"
New-Item -ItemType Directory -Force -Path $assetDir | Out-Null

$sizes = @(
    @{ Name = "StoreLogo.png";         W = 50;  H = 50  },
    @{ Name = "Square44x44Logo.png";   W = 44;  H = 44  },
    @{ Name = "Square150x150Logo.png"; W = 150; H = 150 },
    @{ Name = "Wide310x150Logo.png";   W = 310; H = 150 },
    @{ Name = "Square310x310Logo.png"; W = 310; H = 310 },
    @{ Name = "SplashScreen.png";      W = 620; H = 300 }
)

$bgColor   = [System.Drawing.ColorTranslator]::FromHtml("#101010")
$textColor = [System.Drawing.ColorTranslator]::FromHtml("#7FD4FF")
$srcImage  = $null
if ($IconSource -ne "" -and (Test-Path $IconSource)) {
    $srcImage = [System.Drawing.Image]::FromFile((Resolve-Path $IconSource).Path)
    Write-Host "icon source: $($srcImage.Width)x$($srcImage.Height) $IconSource"
}
if ($IconLabel -eq "") { $IconLabel = "SR" }

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s.W, $s.H)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.Clear($bgColor)

    if ($srcImage) {
        # contain-fit, preserving aspect ratio
        $scale = [Math]::Min($s.W / $srcImage.Width, $s.H / $srcImage.Height)
        $w = [int]($srcImage.Width * $scale); $h = [int]($srcImage.Height * $scale)
        $x = [int](($s.W - $w) / 2); $y = [int](($s.H - $h) / 2)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($srcImage, $x, $y, $w, $h)
    } else {
        $fontSize = [Math]::Max(10, [int]($s.H * 0.34))
        $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $fmt = New-Object System.Drawing.StringFormat
        $fmt.Alignment = [System.Drawing.StringAlignment]::Center
        $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
        $rect = New-Object System.Drawing.RectangleF(0, 0, $s.W, $s.H)
        $brush = New-Object System.Drawing.SolidBrush($textColor)
        $g.DrawString($IconLabel, $font, $brush, $rect, $fmt)
        $brush.Dispose(); $font.Dispose(); $fmt.Dispose()
    }

    $g.Dispose()
    $out = Join-Path $assetDir $s.Name
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("  {0,-24} {1}x{2}" -f $s.Name, $s.W, $s.H)
}
if ($srcImage) { $srcImage.Dispose() }

# ---------------------------------------------------------------- resources

# A manifest that declares <Resources> expects a resource map index. Shipping without
# resources.pri is a classic "deploys fine, fails to launch" packaging bug, so it is
# generated here with the SDK's makepri rather than left to chance.
Write-Section "Generating resources.pri"
$makepri  = Find-SdkTool "makepri.exe"
$priConfig = Join-Path $stage "priconfig.xml"
$priOut    = Join-Path $stage "resources.pri"

& $makepri createconfig /cf $priConfig /dq en-US /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed with exit code $LASTEXITCODE" }

& $makepri new /pr $stage /cf $priConfig /of $priOut /in $IdentityName /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri new failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $priOut)) { throw "makepri reported success but produced no $priOut" }

# The config file is an input, not package content.
Remove-Item -Force $priConfig
Write-Host "resource index: $priOut ($((Get-Item $priOut).Length) bytes)"

# ---------------------------------------------------------------- pack

Write-Section "Packing with makeappx"
$msixName = "$PackageName`_$($Version)_x64.msix"
$msixPath = Join-Path $OutputDir $msixName
if (Test-Path $msixPath) { Remove-Item -Force $msixPath }

& $makeappx pack /o /d $stage /p $msixPath
if ($LASTEXITCODE -ne 0) { throw "makeappx failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $msixPath)) { throw "makeappx reported success but $msixPath does not exist" }
Write-Host "package: $msixPath ($([Math]::Round((Get-Item $msixPath).Length / 1MB, 2)) MB)"

# ---------------------------------------------------------------- sign

if (-not $SkipSign) {
    Write-Section "Signing"
    if ($CertPfx -eq "" -or -not (Test-Path $CertPfx)) {
        throw "CertPfx is required for Xbox deployment (run New-DevCert.ps1 first). Path given: '$CertPfx'"
    }
    & $signtool sign /fd SHA256 /f $CertPfx /p $CertPassword /a $msixPath
    if ($LASTEXITCODE -ne 0) { throw "signtool failed with exit code $LASTEXITCODE" }
    & $signtool verify /pa /all $msixPath
    if ($LASTEXITCODE -ne 0) {
        throw ("signtool verify failed with exit code $LASTEXITCODE. " +
               "A 'root certificate which is not trusted' result means the signing " +
               "certificate is not trusted on this machine; on a build machine add it to " +
               "Cert:\CurrentUser\TrustedPeople (and Root) first.")
    }

    # The package only deploys if the signing certificate's subject matches the manifest
    # Identity/@Publisher exactly. A mismatch fails on the console with 0x80073CF3 and is
    # otherwise very hard to read from the deployment error.
    $sig = Get-AuthenticodeSignature -FilePath $msixPath
    $actualPublisher = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject.Trim() } else { '' }
    if ($actualPublisher -ine $Publisher.Trim()) {
        throw "Publisher mismatch: manifest says '$Publisher' but the package is signed as '$actualPublisher'. The console would reject this with 0x80073CF3."
    }
    Write-Host "publisher OK: $actualPublisher"
    Write-Host "status      : $($sig.Status)" 

    # Export the public certificate (the .cer the console has to trust).
    # signtool has no "export" verb, so this is done with the .NET certificate API.
    $cerOut = Join-Path $OutputDir "$PackageName.cer"
    if (Test-Path $cerOut) { Remove-Item -Force $cerOut }
    $signingCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
        (Resolve-Path $CertPfx).Path, $CertPassword)
    [System.IO.File]::WriteAllBytes(
        $cerOut,
        $signingCert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    Write-Host "certificate: $cerOut (install this on the console)"
    Write-Host "  subject : $($signingCert.Subject)"
} else {
    Write-Host "SIGNING SKIPPED (-SkipSign): the package will not deploy to a console."
    $cerOut = $null
}

# ---------------------------------------------------------------- package zip

Write-Section "Writing $ArtifactName.zip"
$zipDir = Join-Path $OutputDir "_zip"
if (Test-Path $zipDir) { Remove-Item -Recurse -Force $zipDir }
New-Item -ItemType Directory -Force -Path $zipDir | Out-Null

Copy-Item $msixPath $zipDir
if ($cerOut -and (Test-Path $cerOut)) { Copy-Item $cerOut $zipDir }

# Framework dependency: without this the app installs and then fails to start because the
# VC++ runtime DLLs are not part of the package (we link /MD, which C++/CX requires).
$vclibs = Find-Vclibs
if ($vclibs) {
    $depDir = Join-Path $zipDir "dependencies"
    New-Item -ItemType Directory -Force -Path $depDir | Out-Null
    Copy-Item $vclibs $depDir
    $vclibsName = Split-Path -Leaf $vclibs
    Write-Host "dependency: $vclibsName (install this before the app on the console)"
} else {
    $vclibsName = $null
    Write-Warning "Microsoft.VCLibs.x64.14.00.appx not found; the package will need the VC++ runtime installed separately."
}

$install = @"
$DisplayName - Xbox UWP package (Developer Mode sideloading only)

Contents
  $msixName$(if ($cerOut) { "`n  $PackageName.cer      signing certificate - trust this on the console first" })$(if ($vclibsName) { "`n  dependencies\$vclibsName`n                        VC++ runtime framework - install BEFORE the app" })

Game code
$(if ($hasGameCode) { "  included: skygame.dll, built from your own game files (id $gameId)" } else { "  NOT INCLUDED - this is a smoke test. It starts, says the package has no game" + "`n  code in it, and exits. Re-package with -GameDir to get a playable build." })

This package is signed with a self-signed certificate. It cannot be installed in
Retail mode, and it cannot be installed on a console that does not trust the
certificate.

Install
  1. Put the console in Developer Mode and open the Xbox Device Portal
     (Dev Home > Settings > Remote Access), normally https://<console-ip>:11443
  2. Device Portal > Certificates (or the sandbox's certificate manager):
     add $PackageName.cer as trusted.
  3. Device Portal > Apps > Add / Register package: if a dependencies folder is present,
     install the Microsoft.VCLibs framework package FIRST, then select $msixName.
  4. Dev Home > the app > View details > App type = Game (needed for the full
     5120 MiB memory budget instead of the small App-mode budget).
  5. Launch from My games & apps inside the Developer Mode sandbox.

Identity
  Name      : $IdentityName
  Publisher : $Publisher
  Version   : $Version
  Arch      : x64
"@
Set-Content -LiteralPath (Join-Path $zipDir "INSTALL.md") -Value $install -Encoding UTF8

$files = Get-ChildItem -Path $zipDir -File -Recurse
$lines = foreach ($f in $files) {
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLower()
    "$h  $($f.FullName.Substring($zipDir.Length).TrimStart('\'))"
}
Set-Content -LiteralPath (Join-Path $zipDir "SHA256SUMS.txt") -Value ($lines -join "`n") -Encoding UTF8

$zipPath = Join-Path $OutputDir "$ArtifactName.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $zipDir '*') -DestinationPath $zipPath -CompressionLevel Optimal

$zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLower()
Write-Host ""
Write-Host "artifact : $zipPath"
Write-Host "size     : $((Get-Item $zipPath).Length) bytes"
Write-Host "sha256   : $zipHash"

if ($env:GITHUB_OUTPUT) {
    "artifact=$zipPath"     | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "sha256=$zipHash"       | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "msix=$msixPath"        | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "has_game_code=$($hasGameCode.ToString().ToLower())" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
