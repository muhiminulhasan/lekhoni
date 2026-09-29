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
$leftovers = $false
if ($resolvedInstallRoot.StartsWith($expectedParent, [StringComparison]::OrdinalIgnoreCase) -and
    (Test-Path -LiteralPath $resolvedInstallRoot)) {
    try {
        Remove-Item -LiteralPath $resolvedInstallRoot -Recurse -Force -ErrorAction Stop
    } catch {
        # The IME DLL is loaded into every running application that typed with
        # it, so locked files are the expected outcome, not an error.
        $leftovers = $true
    }
}
if (Test-Path -LiteralPath $resolvedInstallRoot) {
    $leftovers = $true
}

# Remove the per-user selection memory for the current user. Other profiles'
# copies are harmless preference files and are left alone.
$localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
$userMemory = Join-Path $localAppData 'Lekhoni'
if ($userMemory.StartsWith((Join-Path $localAppData '').TrimEnd('\') + '\',
        [StringComparison]::OrdinalIgnoreCase) -and
    (Test-Path -LiteralPath $userMemory)) {
    Remove-Item -LiteralPath $userMemory -Recurse -Force -ErrorAction SilentlyContinue
}

if ($leftovers) {
    # A SYSTEM task at next sign-in can delete files no running process holds
    # any more; nothing loads the DLL after this script, because both
    # architectures were unregistered above.
    $cleanupDirectory = Join-Path ([Environment]::GetFolderPath(
        [Environment+SpecialFolder]::CommonApplicationData)) 'LekhoniCleanup'
    $cleanupScript = Join-Path $cleanupDirectory 'cleanup.cmd'
    New-Item -ItemType Directory -Path $cleanupDirectory -Force | Out-Null
    @(
        '@echo off'
        "rd /s /q `"$resolvedInstallRoot`""
        'schtasks /delete /tn "LekhoniCleanup" /f >nul 2>&1'
        "rd /s /q `"$cleanupDirectory`""
    ) | Set-Content -LiteralPath $cleanupScript -Encoding Ascii
    schtasks.exe /create /tn 'LekhoniCleanup' /tr "`"$cleanupScript`"" /sc onlogon /ru SYSTEM /f | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Could not schedule the logon cleanup; remove $resolvedInstallRoot manually after signing out."
    } else {
        Write-Host 'Some files were still loaded by running applications.'
        Write-Host 'A scheduled task (LekhoniCleanup) will delete the leftovers at the next sign-in.'
    }
}

$answer = Read-Host 'Sign out now to refresh the language list? [Y/N]'
if ($answer -match '^\s*[Yy]') {
    Write-Host 'Signing out...'
    logoff.exe
} else {
    Write-Host 'Lekhoni unregistered and removed. Sign out and back in to refresh the language list.'
}
