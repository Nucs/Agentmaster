#Requires -Version 7
<#
.SYNOPSIS
    Install QA kit (PROFILES.md section 2c): make a SIGNED copy of a build's .msix at a chosen version, so a test
    desktop can install the MSIX package and run the app's own package update against a published release.

.DESCRIPTION
    The build's .msix (src\cascadia\CascadiaPackage\AppPackages\...\CascadiaPackage_<ver>_x64.msix, written by
    a Release build of the release identity) is unsigned, and Windows installs only a signed package whose
    signer it trusts. This script:

      1. makes a THROWAWAY code-signing certificate "CN=Agentmaster" (the manifest's Publisher - a package
         signature is valid only when the signer's subject equals the Identity Publisher) in memory with
         .NET's CertificateRequest: no certificate store of this PC is touched, and its private key never
         leaves this process except as a temporary .pfx that is deleted before the script ends;
      2. unpacks the .msix (makeappx), sets the manifest's Identity Version, packs it again and signs it.

    Output in -OutDir: Agentmaster_<Version>_x64.msix and qa-signing.cer (the public certificate). In the
    test desktop, trust the .cer for the whole machine and add the package with its WinUI dependency:

        Import-Certificate -FilePath qa-signing.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
        Add-AppxPackage -Path Agentmaster_0.7.0.0_x64.msix -DependencyPath Microsoft.UI.Xaml.2.8.appx

    Stamp a version OLDER than the newest published release: the app's update then offers that release, and
    its certificate question (the release's own certificate is not trusted yet) runs for real.

    Run it on a development PC only; the package it makes is for a disposable test desktop.

.PARAMETER Msix
    The build's unsigned .msix.

.PARAMETER OutDir
    The folder that receives the signed .msix and the .cer (created when missing; an earlier 'unpacked'
    work folder in it is replaced).

.PARAMETER Version
    The Identity Version to stamp: four numeric parts, e.g. 0.7.0.0.

.OUTPUTS
    None. Prints the certificate's thumbprint, the new Identity element and the signed package's path.

.NOTES
    Throws (and leaves no .pfx behind) when no Windows SDK makeappx.exe / signtool.exe is found, when the
    version is malformed, when the manifest has no Identity Version to replace, or when makeappx or signtool
    fails. The .pfx password is a random value that stays in this process: never printed, never written.
#>
param(
    [Parameter(Mandatory)][string]$Msix,
    [Parameter(Mandatory)][string]$OutDir,
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+\.\d+$')][string]$Version
)
$ErrorActionPreference = 'Stop'

<#
.SYNOPSIS
    The x64 bin folder of the newest installed Windows 10/11 SDK that has both makeappx.exe and signtool.exe.
.OUTPUTS
    [string] the folder.
.NOTES
    Throws when no installed SDK has both tools.
#>
function Find-SdkBin {
    $root = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidates = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending)
    foreach ($c in $candidates) {
        $bin = Join-Path $c.FullName 'x64'
        if ((Test-Path -LiteralPath (Join-Path $bin 'makeappx.exe')) -and (Test-Path -LiteralPath (Join-Path $bin 'signtool.exe'))) {
            return $bin
        }
    }
    throw "no Windows SDK with makeappx.exe and signtool.exe under $root"
}

$sdk = Find-SdkBin
$makeappx = Join-Path $sdk 'makeappx.exe'
$signtool = Join-Path $sdk 'signtool.exe'
if (-not (Test-Path -LiteralPath $Msix)) { throw "no such package: $Msix" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---- the certificate (in memory) ----
# Key usage DigitalSignature + the code-signing EKU (1.3.6.1.5.5.7.3.3) + "not a CA": what signtool and
# the package installer require of a package signer. Valid from yesterday (clock skew) for 30 days.
$rsa = [System.Security.Cryptography.RSA]::Create(2048)
$req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=Agentmaster', $rsa,
    [System.Security.Cryptography.HashAlgorithmName]::SHA256, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
$req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
    [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
$eku = [System.Security.Cryptography.OidCollection]::new()
$null = $eku.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
$req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
$req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
$cert = $req.CreateSelfSigned([DateTimeOffset]::Now.AddDays(-1), [DateTimeOffset]::Now.AddDays(30))
$pw = [guid]::NewGuid().ToString('N')
$pfx = Join-Path $OutDir 'qa-signing.pfx'
$cer = Join-Path $OutDir 'qa-signing.cer'
$work = Join-Path $OutDir 'unpacked'
# signtool needs the private key as a file; the finally block removes it on every path, a failure included.
try {
    [IO.File]::WriteAllBytes($pfx, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $pw))
    [IO.File]::WriteAllBytes($cer, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    Write-Host "certificate: CN=Agentmaster, thumbprint $($cert.Thumbprint)"

    # ---- unpack, set the version, pack, sign ----
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    & $makeappx unpack /p $Msix /d $work /o /l | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makeappx unpack failed ($LASTEXITCODE)" }
    # makeappx writes these itself on pack and refuses a folder that already holds them.
    foreach ($n in 'AppxBlockMap.xml', '[Content_Types].xml', 'AppxSignature.p7x') {
        $p = Join-Path $work $n
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    }
    $meta = Join-Path $work 'AppxMetadata'
    if (Test-Path -LiteralPath $meta) { Remove-Item -LiteralPath $meta -Recurse -Force }
    $manifest = Join-Path $work 'AppxManifest.xml'
    $xml = [IO.File]::ReadAllText($manifest)
    # Only the Version attribute of the Identity element: other elements carry Version attributes too.
    $new = [regex]::Replace($xml, '(<Identity\b[^>]*?\bVersion=")[^"]*(")', "`${1}$Version`${2}", 'Singleline')
    if ($new -eq $xml -and $xml -notmatch "Version=`"$([regex]::Escape($Version))`"") { throw 'the Identity Version was not found in AppxManifest.xml' }
    [IO.File]::WriteAllText($manifest, $new, (New-Object Text.UTF8Encoding($false)))
    Write-Host ("identity: " + [regex]::Match($new, '<Identity\b[^>]*>', 'Singleline').Value)
    $out = Join-Path $OutDir ("Agentmaster_" + $Version + "_x64.msix")
    & $makeappx pack /d $work /p $out /o /l | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed ($LASTEXITCODE)" }
    & $signtool sign /fd SHA256 /f $pfx /p $pw $out | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool sign failed ($LASTEXITCODE)" }
    Write-Host ("signed: {0} ({1:N1} MB)" -f $out, ((Get-Item -LiteralPath $out).Length / 1MB))
} finally {
    if (Test-Path -LiteralPath $pfx) { Remove-Item -LiteralPath $pfx -Force }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
