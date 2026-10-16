#Requires -Version 5.1
<#
.SYNOPSIS
Signs a release EXE with an existing CurrentUser\My code-signing certificate.
.DESCRIPTION
Requires Windows SDK SignTool, SHA-256, an HTTPS RFC 3161 timestamp, and a
Windows-trusted Authenticode result. Signs and verifies a temporary sibling
copy before atomically replacing the input. Does not create/import certificates,
export private keys, modify trust stores, or support unsigned fallback.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$CertificateThumbprint,

    [uri]$TimestampUrl = 'https://timestamp.digicert.com',

    [string]$SignToolPath,

    [switch]$ReplaceExistingSignature
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-SignTool {
    param([string]$ExplicitPath)
    if ($ExplicitPath) {
        $tool = Get-Item -LiteralPath $ExplicitPath -ErrorAction Stop
        if ($tool.PSIsContainer -or $tool.Name -ine 'signtool.exe') {
            throw 'SignToolPath must identify signtool.exe.'
        }
        return $tool.FullName
    }

    $command = Get-Command signtool.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($command) { return $command.Source }

    $roots = @()
    foreach ($registryPath in @(
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
    )) {
        $installed = Get-ItemProperty -LiteralPath $registryPath -ErrorAction SilentlyContinue
        if ($installed -and $installed.PSObject.Properties['KitsRoot10']) {
            $roots += [string]$installed.KitsRoot10
        }
    }
    if (${env:ProgramFiles(x86)}) {
        $roots += Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    }
    if ($env:ProgramFiles) {
        $roots += Join-Path $env:ProgramFiles 'Windows Kits\10'
    }

    $architectures = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
        @('arm64', 'x64', 'x86')
    } elseif ([Environment]::Is64BitOperatingSystem) {
        @('x64', 'x86')
    } else { @('x86') }

    foreach ($root in @($roots | Select-Object -Unique)) {
        $bin = Join-Path $root 'bin'
        if (-not (Test-Path -LiteralPath $bin -PathType Container)) { continue }
        $versions = @(Get-ChildItem -LiteralPath $bin -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending)
        $directories = @($versions | ForEach-Object { $_.FullName }) + @($bin)
        foreach ($directory in $directories) {
            foreach ($architecture in $architectures) {
                $candidate = Join-Path $directory "$architecture\signtool.exe"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
            }
        }
    }
    throw 'Windows SDK signtool.exe was not found. Install Windows SDK Signing Tools or supply -SignToolPath.'
}

function Invoke-SignTool {
    param([string]$Tool, [string[]]$Arguments, [string]$Operation)
    $output = @(& $Tool @Arguments 2>&1)
    $result = $LASTEXITCODE
    foreach ($line in $output) { Write-Host $line.ToString() }
    # SignTool uses 2 for warnings, including some timestamp failures. A release
    # must pass without warnings; never silently continue with a partial result.
    if ($result -ne 0) { throw "$Operation failed (SignTool exit code $result). The input EXE was not replaced." }
}

function Assert-ReleaseSignature {
    param([string]$FilePath, [string]$ExpectedThumbprint)
    $signature = Get-AuthenticodeSignature -LiteralPath $FilePath
    if ($signature.Status.ToString() -ne 'Valid') {
        throw "Windows Authenticode verification failed: $($signature.Status). $($signature.StatusMessage)"
    }
    if (-not $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $ExpectedThumbprint) {
        throw 'The verified signer does not match the explicitly selected certificate.'
    }
    if (-not $signature.TimeStamperCertificate) {
        throw 'No verified timestamp certificate was found. A release timestamp is required.'
    }
    return $signature
}

if ($env:OS -ne 'Windows_NT') { throw 'This signing script requires Windows.' }
if (-not $TimestampUrl.IsAbsoluteUri -or $TimestampUrl.Scheme -ne 'https' -or
    $TimestampUrl.UserInfo -or $TimestampUrl.Query -or $TimestampUrl.Fragment) {
    throw 'TimestampUrl must be an absolute HTTPS RFC 3161 endpoint without credentials, query parameters, or fragments.'
}

$inputFile = Get-Item -LiteralPath $Path
if ($inputFile.PSIsContainer -or $inputFile.Extension -ne '.exe') {
    throw 'Path must identify an existing release .exe file.'
}
$inputPath = $inputFile.FullName
$thumbprint = ($CertificateThumbprint -replace '[\s\u200e\u200f]', '').ToUpperInvariant()
if ($thumbprint -notmatch '^[0-9A-F]{40}$') {
    throw 'CertificateThumbprint must contain the 40 hexadecimal characters shown by the Windows certificate store.'
}

$certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$thumbprint" -ErrorAction SilentlyContinue
if (-not $certificate) {
    throw 'The selected certificate is not installed in Cert:\CurrentUser\My. No certificate was created or imported.'
}
if (-not $certificate.HasPrivateKey) { throw 'The selected certificate has no accessible private key.' }
$now = Get-Date
if ($now -lt $certificate.NotBefore -or $now -ge $certificate.NotAfter) {
    throw 'The selected certificate is not currently within its validity period.'
}
$codeSigningEku = $false
foreach ($extension in $certificate.Extensions) {
    if ($extension.Oid.Value -eq '2.5.29.37') {
        foreach ($usage in $extension.EnhancedKeyUsages) {
            if ($usage.Value -eq '1.3.6.1.5.5.7.3.3') { $codeSigningEku = $true }
        }
    }
    if ($extension.Oid.Value -eq '2.5.29.15' -and
        -not ($extension.KeyUsages -band [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature)) {
        throw 'The certificate key usage does not permit digital signatures.'
    }
}
if (-not $codeSigningEku) { throw 'The selected certificate lacks the Code Signing EKU (1.3.6.1.5.5.7.3.3).' }

$existing = Get-AuthenticodeSignature -LiteralPath $inputPath
if ($existing.Status.ToString() -ne 'NotSigned' -and -not $ReplaceExistingSignature) {
    throw 'The EXE already contains a signature or cannot be classified as unsigned. Use a fresh build, or explicitly supply -ReplaceExistingSignature.'
}
$signTool = Find-SignTool -ExplicitPath $SignToolPath
if (-not $PSCmdlet.ShouldProcess($inputPath, "Sign with certificate $thumbprint; require trusted SHA-256 signature and RFC 3161 timestamp")) {
    return
}

$originalHash = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
$stagingPath = Join-Path $inputFile.DirectoryName (
    '.' + $inputFile.BaseName + '.signing.' + [guid]::NewGuid().ToString('N') + '.exe')
try {
    Copy-Item -LiteralPath $inputPath -Destination $stagingPath -ErrorAction Stop
    Invoke-SignTool -Tool $signTool -Operation 'Signing/timestamping' -Arguments @(
        'sign', '/s', 'My', '/sha1', $thumbprint,
        '/fd', 'SHA256', '/tr', $TimestampUrl.AbsoluteUri, '/td', 'SHA256',
        $stagingPath
    )
    Invoke-SignTool -Tool $signTool -Operation 'Authenticode verification' -Arguments @(
        'verify', '/pa', '/all', '/tw', '/v', $stagingPath
    )
    $null = Assert-ReleaseSignature -FilePath $stagingPath -ExpectedThumbprint $thumbprint
    if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -ne $originalHash) {
        throw 'The input EXE changed while signing. Stop concurrent builds and try again; it was not replaced.'
    }

    # The temporary file is a sibling, so replacement stays on one filesystem.
    # A lock or replacement failure leaves the original in place and is an error.
    [System.IO.File]::Replace($stagingPath, $inputPath, [System.Management.Automation.Language.NullString]::Value)
    $verified = Assert-ReleaseSignature -FilePath $inputPath -ExpectedThumbprint $thumbprint
    [pscustomobject]@{
        Path = $inputPath
        AuthenticodeStatus = $verified.Status.ToString()
        SignerSubject = $verified.SignerCertificate.Subject
        SignerThumbprint = $verified.SignerCertificate.Thumbprint
        TimestampSubject = $verified.TimeStamperCertificate.Subject
        SHA256 = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
    }
} finally {
    # Delete only the single temporary file created by this invocation.
    if (Test-Path -LiteralPath $stagingPath -PathType Leaf) {
        Remove-Item -LiteralPath $stagingPath -Force
    }
}
