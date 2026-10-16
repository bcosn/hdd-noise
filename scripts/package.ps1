#Requires -Version 5.1
<#
.SYNOPSIS
Creates a portable Windows x64 ZIP and SHA256SUMS.txt from a finished EXE.
.DESCRIPTION
Packages an explicit file list. Signing must happen before packaging. This
script records the actual Windows signature result; it does not sign files.
#>
[CmdletBinding()]
param(
    [string]$Executable = 'out/hdd-noise.exe',
    [string]$Destination = 'out/release',
    [string]$Version = '0.1.1'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Package the Windows executable on Windows so Authenticode can be checked.' }
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$') { throw 'Invalid release version.' }

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$inputFile = Get-Item -LiteralPath $Executable
if ($inputFile.PSIsContainer -or $inputFile.Extension -ine '.exe') { throw 'Executable must identify an existing EXE.' }
$exePath = $inputFile.FullName

# Fail rather than mislabel an x86/ARM build as the x64 release.
$stream = [IO.File]::OpenRead($exePath)
$reader = New-Object IO.BinaryReader($stream)
try {
    if ($reader.ReadUInt16() -ne 0x5a4d) { throw 'Input has no DOS/PE executable header.' }
    $stream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    if ($peOffset -lt 0x40 -or $peOffset -gt $stream.Length - 6) { throw 'Invalid PE header offset.' }
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
        throw 'This package requires a Windows x64 (AMD64) executable.'
    }
} finally { $reader.Dispose(); $stream.Dispose() }

$fileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($exePath)
if ($fileVersion.ProductVersion -ne ($Version -split '-', 2)[0]) {
    throw 'The requested version does not match the EXE ProductVersion resource. Rebuild the matching version first.'
}
$signature = Get-AuthenticodeSignature -LiteralPath $exePath
$signatureStatus = $signature.Status.ToString()
if ($signatureStatus -notin @('NotSigned', 'Valid')) {
    throw "Refusing to package an invalid or untrusted signature ($signatureStatus). Use a fresh unsigned build or a properly verified signed EXE."
}
$exeHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
$releaseInfo = [ordered]@{
    formatVersion = 1
    product = 'hdd-noise'
    version = $Version
    architecture = 'windows-x64'
    executable = 'hdd-noise.exe'
    executableSHA256 = $exeHash
    authenticode = [ordered]@{
        status = $signatureStatus
        signerSubject = $(if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { $null })
        signerThumbprint = $(if ($signature.SignerCertificate) { $signature.SignerCertificate.Thumbprint } else { $null })
        timestamped = [bool]$signature.TimeStamperCertificate
    }
    build = [ordered]@{
        repository = $env:GITHUB_REPOSITORY
        commit = $env:GITHUB_SHA
        runId = $env:GITHUB_RUN_ID
        runAttempt = $env:GITHUB_RUN_ATTEMPT
        packagedAtUtc = [DateTime]::UtcNow.ToString('o')
    }
}
$files = [ordered]@{
    'hdd-noise.exe' = $exePath
    'README.md' = Join-Path $projectRoot 'README.md'
    'SOUND-DESIGN.md' = Join-Path $projectRoot 'SOUND-DESIGN.md'
    'docs/SIGNING.md' = Join-Path $projectRoot 'docs/SIGNING.md'
    'docs/RELEASING.md' = Join-Path $projectRoot 'docs/RELEASING.md'
    'scripts/prepare-desktop.ps1' = Join-Path $projectRoot 'scripts/prepare-desktop.ps1'
}
if (Test-Path -LiteralPath (Join-Path $projectRoot 'LICENSE') -PathType Leaf) {
    $files['LICENSE'] = Join-Path $projectRoot 'LICENSE'
}
foreach ($source in $files.Values) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required release file is missing: $source" }
}

$destinationPath = [IO.Path]::GetFullPath($Destination)
[IO.Directory]::CreateDirectory($destinationPath) | Out-Null
$zipName = "hdd-noise-$Version-windows-x64.zip"
$zipPath = Join-Path $destinationPath $zipName
$temporaryZip = Join-Path $destinationPath ('.hdd-noise.' + [guid]::NewGuid().ToString('N') + '.zip')
$utf8 = New-Object Text.UTF8Encoding($false)
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
try {
    $zip = [IO.Compression.ZipFile]::Open($temporaryZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $files.Keys) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $files[$name], $name,
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
        $entry = $zip.CreateEntry('release-info.json')
        $writer = New-Object IO.StreamWriter($entry.Open(), $utf8)
        try { $writer.Write(($releaseInfo | ConvertTo-Json -Depth 6)) } finally { $writer.Dispose() }
    } finally { $zip.Dispose() }

    if ((Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $exeHash) {
        throw 'The EXE changed during packaging. Stop concurrent builds/signing and retry.'
    }
    if (Test-Path -LiteralPath $zipPath -PathType Leaf) {
        [IO.File]::Replace($temporaryZip, $zipPath, $null)
    } else {
        [IO.File]::Move($temporaryZip, $zipPath)
    }
    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $destinationPath 'SHA256SUMS.txt'), "$zipHash  $zipName`n", [Text.Encoding]::ASCII)
    [pscustomobject]@{ Package = $zipPath; SHA256 = $zipHash; AuthenticodeStatus = $signatureStatus }
} finally {
    # No recursive deletion: remove only this invocation's temporary ZIP.
    if (Test-Path -LiteralPath $temporaryZip -PathType Leaf) { Remove-Item -LiteralPath $temporaryZip -Force }
}
