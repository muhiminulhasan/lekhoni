[CmdletBinding()]
param(
    [string] $DictionarySource,
    [switch] $UseStarterDictionary
)

$ErrorActionPreference = 'Stop'
if ($DictionarySource -and $UseStarterDictionary) {
    throw 'Use either -DictionarySource or -UseStarterDictionary, not both.'
}
if ($DictionarySource) {
    Write-Host "Custom dictionary source will be included in the package: $DictionarySource"
} elseif ($UseStarterDictionary) {
    Write-Host 'The project starter dictionary will be included in the package.'
} else {
    Write-Host 'The default key-augmented merged dictionary will be included in the package.'
    # The default dictionary (data/source/imported-core.tsv, compiled from the merged
    # corpus) and the Avro layout table are third-party-derived (GPL/MPL) and are
    # licensed under their upstream licenses, not the project MIT license, so a
    # package containing them may only be distributed in compliance with those
    # licenses.
    Write-Warning @'
This package contains dictionary data derived from Avro Keyboard (GPL),
Lekho (MPL-2.0), and Bengali Wikipedia (CC BY-SA 3.0/GFDL), plus the Avro
Phonetic layout table (GPL). Those files are licensed under their upstream
licenses, not the project MIT license. The archive therefore ships the
compliance set those licenses require: the upstream license texts under
third-party-licenses\, the attribution notices in THIRD_PARTY_NOTICES.md,
and the source-form TSVs under source\. Keep all three with any
redistribution of the derived files.

Use -UseStarterDictionary for a package covered by the MIT License alone,
or pass -DictionarySource with a corpus you are authorized to redistribute.
See data/README.md.
'@
}
$root = $PSScriptRoot
$artifacts = Join-Path $root 'artifacts'
$package = Join-Path $artifacts 'Lekhoni-win-x86-x64'
$archive = "$package.zip"

# Always rebuild so a package cannot silently contain stale binaries.
$buildArguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'build.ps1'),
    '-Configuration', 'Release', '-Platform', 'Both', '-Clean', '-RunTests', '-BuildDictionary'
)
if ($DictionarySource) {
    $buildArguments += @('-DictionarySource', $DictionarySource)
} elseif ($UseStarterDictionary) {
    $buildArguments += '-UseStarterDictionary'
}
& powershell @buildArguments
if ($LASTEXITCODE -ne 0) {
    throw 'Release build failed.'
}

if (Test-Path -LiteralPath $package) {
    Remove-Item -LiteralPath $package -Recurse -Force
}
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}

New-Item -ItemType Directory -Path (Join-Path $package 'bin\Win32') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $package 'bin\x64') -Force | Out-Null

foreach ($architecture in @('Win32', 'x64')) {
    $source = Join-Path $artifacts "$architecture\Release"
    Copy-Item -LiteralPath (Join-Path $source 'LekhoniIME.dll') -Destination (Join-Path $package "bin\$architecture\LekhoniIME.dll")
    Copy-Item -LiteralPath (Join-Path $source 'LekhoniRegistration.exe') -Destination (Join-Path $package "bin\$architecture\LekhoniRegistration.exe")
    Copy-Item -LiteralPath (Join-Path $artifacts 'LekhoniCore.bgk') -Destination (Join-Path $package "bin\$architecture\LekhoniCore.bgk")

    # The phrase layer and the trigram table are optional runtime data: a
    # missing image only disables that feature, so it is copied when present.
    $phraseImage = Join-Path $artifacts 'LekhoniPhrases.bgk'
    if (Test-Path -LiteralPath $phraseImage -PathType Leaf) {
        Copy-Item -LiteralPath $phraseImage -Destination (Join-Path $package "bin\$architecture\LekhoniPhrases.bgk")
    }
    $trigramImage = Join-Path $artifacts 'LekhoniTrigrams.dat'
    if (Test-Path -LiteralPath $trigramImage -PathType Leaf) {
        Copy-Item -LiteralPath $trigramImage -Destination (Join-Path $package "bin\$architecture\LekhoniTrigrams.dat")
    }

    # The Avro layout table is required in the default package: without it the
    # IME silently loses Avro Phonetic fidelity. A starter build omits it on
    # purpose (MIT-only data), so its absence is expected there.
    $layoutImage = Join-Path $artifacts 'avro-layout.dat'
    if (Test-Path -LiteralPath $layoutImage -PathType Leaf) {
        Copy-Item -LiteralPath $layoutImage -Destination (Join-Path $package "bin\$architecture\AvroLayout.dat")
    } elseif (-not $UseStarterDictionary) {
        throw 'Avro layout table missing from build output; the package would lose Avro compatibility.'
    }
}

# The double-clickable installer. Only the 64-bit build ships as the setup
# executable (it must write to the native Program Files); 32-bit Windows
# installs from the same package with install.ps1.
$setupImage = Join-Path $artifacts 'x64\Release\LekhoniSetup.exe'
if (-not (Test-Path -LiteralPath $setupImage -PathType Leaf)) {
    throw 'LekhoniSetup.exe missing from build output; the package needs the installer.'
}
Copy-Item -LiteralPath $setupImage -Destination (Join-Path $package 'LekhoniSetup.exe')

Copy-Item -LiteralPath (Join-Path $root 'install.ps1') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'uninstall.ps1') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'RELEASE_CHECKLIST.md') -Destination $package
# The license must travel with every distributed build, and it names the
# dictionary carve-out, so a recipient can see what is and is not covered.
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $package

# The default package redistributes third-party-derived data, so it must
# carry the upstream license texts, the attribution notices, and the
# source-form TSVs (the GPL "corresponding source" of the compiled
# dictionaries). A starter package contains none of that data and stays
# MIT-only.
if (-not $UseStarterDictionary -and -not $DictionarySource) {
    Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $package
    New-Item -ItemType Directory -Path (Join-Path $package 'third-party-licenses') -Force | Out-Null
    # -Path (not -LiteralPath): the wildcard must expand to the license texts.
    Copy-Item -Path (Join-Path $root 'third-party-licenses\*') `
        -Destination (Join-Path $package 'third-party-licenses')

    New-Item -ItemType Directory -Path (Join-Path $package 'source') -Force | Out-Null
    foreach ($name in @('imported-core.tsv', 'imported-keys.tsv',
                        'imported-autodict.tsv', 'imported-autodict-caps.tsv')) {
        $tsv = Join-Path $root "data\source\$name"
        if (Test-Path -LiteralPath $tsv -PathType Leaf) {
            Copy-Item -LiteralPath $tsv -Destination (Join-Path $package "source\$name")
        } else {
            throw "Source-form TSV missing: $tsv; the package cannot meet the GPL source requirement."
        }
    }
}

Compress-Archive -Path (Join-Path $package '*') -DestinationPath $archive -CompressionLevel Optimal
$archiveInfo = Get-Item -LiteralPath $archive
Write-Host "Created $($archiveInfo.FullName) ($($archiveInfo.Length) bytes)."

# The lite variant is the same package without the trigram next-word table
# (the largest single data file): users who do not want the prediction
# corpus can pick a much smaller download. Everything else - binaries,
# dictionaries, layout table, installer, and the full GPL compliance set -
# is identical, so the choice costs no features besides next-word
# prediction. Starter builds carry no trigram table in the first place, so
# a lite variant would only duplicate the archive.
$liteTrigram = Join-Path $package 'bin\x64\LekhoniTrigrams.dat'
if (Test-Path -LiteralPath $liteTrigram -PathType Leaf) {
    $litePackage = Join-Path $artifacts 'Lekhoni-win-x86-x64-lite'
    $liteArchive = "$litePackage.zip"
    if (Test-Path -LiteralPath $litePackage) {
        Remove-Item -LiteralPath $litePackage -Recurse -Force
    }
    if (Test-Path -LiteralPath $liteArchive) {
        Remove-Item -LiteralPath $liteArchive -Force
    }
    Copy-Item -LiteralPath $package -Destination $litePackage -Recurse
    foreach ($architecture in @('Win32', 'x64')) {
        Remove-Item -LiteralPath (Join-Path $litePackage "bin\$architecture\LekhoniTrigrams.dat") -Force
    }
    Compress-Archive -Path (Join-Path $litePackage '*') -DestinationPath $liteArchive -CompressionLevel Optimal
    $liteInfo = Get-Item -LiteralPath $liteArchive
    Write-Host "Created $($liteInfo.FullName) ($($liteInfo.Length) bytes, no trigram table)."
}
