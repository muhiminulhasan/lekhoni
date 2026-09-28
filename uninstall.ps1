[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell session.'
}

$root = $PSScriptRoot
$artifacts = Join-Path $root 'artifacts'
$programFiles = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
$installRoot = Join-Path $programFiles 'Lekhoni'

$foundRegistrationTool = $false
foreach ($architecture in @('Win32', 'x64')) {
    if ($architecture -eq 'x64' -and -not [Environment]::Is64BitOperatingSystem) {
        continue
    }
    $artifactRegistration = Join-Path $artifacts "$architecture\Release\LekhoniRegistration.exe"
    $packageRegistration = Join-Path $root "bin\$architecture\LekhoniRegistration.exe"
    $registration = if (Test-Path -LiteralPath $artifactRegistration -PathType Leaf) {
        $artifactRegistration
    } else {
        $packageRegistration
    }
    if (Test-Path -LiteralPath $registration -PathType Leaf) {
        $foundRegistrationTool = $true
        $installedDirectory = if ($architecture -eq 'Win32') { 'bin32' } else { 'bin64' }
        & $registration unregister (Join-Path $installRoot "$installedDirectory\LekhoniIME.dll")
        if ($LASTEXITCODE -ne 0) {
            throw "TSF unregistration failed for $architecture."
        }
    }
}
if (-not $foundRegistrationTool) {
    throw 'No registration helper was found; refusing to delete an installed IME blindly.'
}

$comKey = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{6E315A4D-B7F3-4C4A-91E2-59A830447D19}'
$comKeyWow = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\Wow6432Node\CLSID\{6E315A4D-B7F3-4C4A-91E2-59A830447D19}'
if ((Test-Path -LiteralPath $comKey) -or (Test-Path -LiteralPath $comKeyWow)) {
    throw 'A COM registration key remains; files were not removed.'
}

$expectedParent = [IO.Path]::GetFullPath($programFiles).TrimEnd('\') + '\'
$resolvedInstallRoot = [IO.Path]::GetFullPath($installRoot)
if ($resolvedInstallRoot.StartsWith($expectedParent, [StringComparison]::OrdinalIgnoreCase) -and
    (Test-Path -LiteralPath $resolvedInstallRoot)) {
    Remove-Item -LiteralPath $resolvedInstallRoot -Recurse -Force
}

Write-Host 'Lekhoni unregistered and removed. Sign out and back in to refresh the language list.'
