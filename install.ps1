[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [string] $DictionaryPath
)

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
$bin32 = Join-Path $installRoot 'bin32'
$bin64 = Join-Path $installRoot 'bin64'

$artifactSource32 = Join-Path $artifacts "Win32\$Configuration"
$artifactSource64 = Join-Path $artifacts "x64\$Configuration"
$source32 = if (Test-Path -LiteralPath $artifactSource32 -PathType Container) {
    $artifactSource32
} else {
    Join-Path $root 'bin\Win32'
}
$source64 = if (Test-Path -LiteralPath $artifactSource64 -PathType Container) {
    $artifactSource64
} else {
    Join-Path $root 'bin\x64'
}
$registration32 = Join-Path $source32 'LekhoniRegistration.exe'
$registration64 = Join-Path $source64 'LekhoniRegistration.exe'
$dll32 = Join-Path $source32 'LekhoniIME.dll'
$dll64 = Join-Path $source64 'LekhoniIME.dll'
$artifactDictionary = Join-Path $artifacts 'LekhoniCore.bgk'
if ($DictionaryPath) {
    $customDictionary = if ([IO.Path]::IsPathRooted($DictionaryPath)) {
        $DictionaryPath
    } else {
        Join-Path $root $DictionaryPath
    }
    $dictionary = (Resolve-Path -LiteralPath $customDictionary -ErrorAction Stop).Path
} elseif (Test-Path -LiteralPath $artifactDictionary -PathType Leaf) {
    $dictionary = $artifactDictionary
} else {
    $dictionary = Join-Path $source32 'LekhoniCore.bgk'
}
$is64BitWindows = [Environment]::Is64BitOperatingSystem

$requiredFiles = @($registration32, $dll32, $dictionary)
if ($is64BitWindows) {
    $requiredFiles += @($registration64, $dll64)
}
foreach ($required in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required build artifact is missing: $required"
    }
}

Write-Host "Using dictionary: $dictionary"

# The curated phrase layer is optional. When it is present it is installed
# beside the dictionary; when it is absent a stale copy from an earlier install
# is removed so the IME cannot keep using an outdated phrase list.
$phrases = Join-Path $artifacts 'LekhoniPhrases.bgk'
if (-not (Test-Path -LiteralPath $phrases -PathType Leaf)) {
    foreach ($candidate in @((Join-Path $source32 'LekhoniPhrases.bgk'),
                             (Join-Path $artifacts 'Win32\Release\LekhoniPhrases.bgk'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $phrases = $candidate
            break
        }
    }
}
$phrases = if (Test-Path -LiteralPath $phrases -PathType Leaf) { $phrases } else { $null }

# The Avro Phonetic layout table ships beside the DLLs (in artifacts for a
# source install, in bin for a ZIP install). It is optional runtime data: a
# missing table only costs Avro-layout fidelity, but a stale one must not
# linger either.
$layout = Join-Path $artifacts 'avro-layout.dat'
if (-not (Test-Path -LiteralPath $layout -PathType Leaf)) {
    foreach ($candidate in @((Join-Path $source32 'AvroLayout.dat'),
                             (Join-Path $source64 'AvroLayout.dat'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $layout = $candidate
            break
        }
    }
}
$layout = if (Test-Path -LiteralPath $layout -PathType Leaf) { $layout } else { $null }

# The trigram next-word table follows the same optional-file pattern.
$trigrams = Join-Path $artifacts 'LekhoniTrigrams.dat'
if (-not (Test-Path -LiteralPath $trigrams -PathType Leaf)) {
    foreach ($candidate in @((Join-Path $source32 'LekhoniTrigrams.dat'),
                             (Join-Path $source64 'LekhoniTrigrams.dat'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $trigrams = $candidate
            break
        }
    }
}
$trigrams = if (Test-Path -LiteralPath $trigrams -PathType Leaf) { $trigrams } else { $null }

New-Item -ItemType Directory -Path $bin32 -Force | Out-Null
Copy-Item -LiteralPath $dll32 -Destination (Join-Path $bin32 'LekhoniIME.dll') -Force
Copy-Item -LiteralPath $dictionary -Destination (Join-Path $bin32 'LekhoniCore.bgk') -Force
if ($layout) {
    Copy-Item -LiteralPath $layout -Destination (Join-Path $bin32 'AvroLayout.dat') -Force
    Write-Host "Using Avro layout table: $layout"
} else {
    Remove-Item -LiteralPath (Join-Path $bin32 'AvroLayout.dat') -Force -ErrorAction SilentlyContinue
    Write-Host 'No Avro layout table found; the built-in rules will be used.'
}
if ($trigrams) {
    Copy-Item -LiteralPath $trigrams -Destination (Join-Path $bin32 'LekhoniTrigrams.dat') -Force
} else {
    Remove-Item -LiteralPath (Join-Path $bin32 'LekhoniTrigrams.dat') -Force -ErrorAction SilentlyContinue
}
if ($phrases) {
    Copy-Item -LiteralPath $phrases -Destination (Join-Path $bin32 'LekhoniPhrases.bgk') -Force
    Write-Host "Using phrase layer: $phrases"
} else {
    Remove-Item -LiteralPath (Join-Path $bin32 'LekhoniPhrases.bgk') -Force -ErrorAction SilentlyContinue
    Write-Host 'No phrase layer found; curated phrases are disabled.'
}

& $registration32 register (Join-Path $bin32 'LekhoniIME.dll')
if ($LASTEXITCODE -ne 0) {
    throw '32-bit TSF registration failed.'
}

if ($is64BitWindows) {
    New-Item -ItemType Directory -Path $bin64 -Force | Out-Null
    Copy-Item -LiteralPath $dll64 -Destination (Join-Path $bin64 'LekhoniIME.dll') -Force
    Copy-Item -LiteralPath $dictionary -Destination (Join-Path $bin64 'LekhoniCore.bgk') -Force
    if ($layout) {
        Copy-Item -LiteralPath $layout -Destination (Join-Path $bin64 'AvroLayout.dat') -Force
    } else {
        Remove-Item -LiteralPath (Join-Path $bin64 'AvroLayout.dat') -Force -ErrorAction SilentlyContinue
    }
    if ($trigrams) {
        Copy-Item -LiteralPath $trigrams -Destination (Join-Path $bin64 'LekhoniTrigrams.dat') -Force
    } else {
        Remove-Item -LiteralPath (Join-Path $bin64 'LekhoniTrigrams.dat') -Force -ErrorAction SilentlyContinue
    }
    if ($phrases) {
        Copy-Item -LiteralPath $phrases -Destination (Join-Path $bin64 'LekhoniPhrases.bgk') -Force
    } else {
        Remove-Item -LiteralPath (Join-Path $bin64 'LekhoniPhrases.bgk') -Force -ErrorAction SilentlyContinue
    }
    & $registration64 register (Join-Path $bin64 'LekhoniIME.dll')
    if ($LASTEXITCODE -ne 0) {
        & $registration64 unregister (Join-Path $bin64 'LekhoniIME.dll') | Out-Null
        throw '64-bit TSF registration failed; the 64-bit registration was rolled back.'
    }
}

Write-Host 'Lekhoni installed. Use Win+Space to select Lekhoni Bangla Phonetic.'
Write-Host 'If it is not listed immediately, sign out and back in once.'
