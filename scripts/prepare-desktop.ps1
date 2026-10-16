#Requires -Version 5.1
<#
.SYNOPSIS
Restores normal desktop integrity for this project's locally built executable.
.DESCRIPTION
Changes only the mandatory integrity label of the explicitly selected hdd-noise
EXE. Does not change directory labels, DACL permissions, UAC, signatures or trust.
Run from an ordinary desktop PowerShell after exiting an older app instance.
#>
[CmdletBinding()]
param([string]$Executable = 'hdd-noise.exe')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if($env:OS -ne 'Windows_NT'){throw 'This helper is for Windows desktop builds.'}
$exeFile = Get-Item -LiteralPath $Executable
if($exeFile.PSIsContainer -or $exeFile.Extension -ine '.exe' -or
   $exeFile.VersionInfo.ProductName -ne 'hdd-noise'){
    throw 'Select an executable built from the hdd-noise project.'
}
$beforeHash = (Get-FileHash -LiteralPath $exeFile.FullName -Algorithm SHA256).Hash
$icaclsPath = Join-Path $env:WINDIR 'System32/icacls.exe'
& $icaclsPath $exeFile.FullName /setintegritylevel M
if($LASTEXITCODE -ne 0){throw 'Could not update the EXE label. Run this helper from an ordinary desktop PowerShell, outside a restricted build process.'}
$afterHash = (Get-FileHash -LiteralPath $exeFile.FullName -Algorithm SHA256).Hash
if($beforeHash -ne $afterHash){throw 'The executable changed during preparation; rebuild and verify it.'}
Write-Output 'Desktop launch label prepared; EXE bytes unchanged. Exit any older instance and reopen the executable.'
