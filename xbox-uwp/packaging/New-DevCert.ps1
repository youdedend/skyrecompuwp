<#
.SYNOPSIS
  Creates a self-signed code-signing certificate for Xbox Developer Mode sideloading.

.DESCRIPTION
  The certificate Subject must be byte-identical to <Identity Publisher="..."> in the
  package manifest, otherwise the deployment fails with 0x800B0109 (untrusted chain) or
  0x80073CF3 (publisher mismatch).

  The same .cer has to be installed as a trusted certificate on the console before the
  package can be registered. See xbox-uwp/README.md.

.EXAMPLE
  pwsh -File xbox-uwp/packaging/New-DevCert.ps1 -OutDir build-xbox-uwp
#>
[CmdletBinding()]
param(
    [string]$Publisher = "CN=SkyRecomp Dev",
    [string]$OutDir    = "build-xbox-uwp",
    [string]$Password  = "skyrecomp",
    [string]$BaseName  = "SkyRecompUwp"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "Creating self-signed code-signing certificate"
Write-Host "  Subject: $Publisher"

$cert = New-SelfSignedCertificate `
    -Type Custom `
    -Subject $Publisher `
    -KeyUsage DigitalSignature `
    -FriendlyName "SkyRecomp Xbox UWP (Developer Mode)" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") `
    -NotAfter (Get-Date).AddYears(2)

$pfxPath = Join-Path $OutDir "$BaseName.pfx"
$cerPath = Join-Path $OutDir "$BaseName.cer"

$secure = ConvertTo-SecureString -String $Password -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $secure | Out-Null
Export-Certificate   -Cert $cert -FilePath $cerPath | Out-Null

Write-Host "  Thumbprint: $($cert.Thumbprint)"
Write-Host "  PFX:        $pfxPath"
Write-Host "  CER:        $cerPath"
Write-Host ""
Write-Host "Install $cerPath as a trusted certificate on the console (Device Portal) before deploying."

# Machine-readable outputs for CI
if ($env:GITHUB_OUTPUT) {
    "pfx=$pfxPath"   | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "cer=$cerPath"   | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "thumbprint=$($cert.Thumbprint)" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
