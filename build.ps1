[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [ValidateSet('Both', 'Win32', 'x64')]
    [string] $Platform = 'Both',

    [switch] $Clean,
    [switch] $RunTests,
    [switch] $BuildDictionary,

    [string] $DictionarySource,
    [switch] $UseStarterDictionary
)

$ErrorActionPreference = 'Stop'
if ($DictionarySource -and $UseStarterDictionary) {
    throw 'Use either -DictionarySource or -UseStarterDictionary, not both.'
}
$root = $PSScriptRoot

# -Clean removes every generated output (intermediate objects, test binaries,
# dictionaries, packages) so the result cannot depend on anything stale. The
# dictionary is rebuilt as part of that, because a cleaned tree has none.
if ($Clean) {
    $artifacts = Join-Path $root 'artifacts'
    if (Test-Path -LiteralPath $artifacts) {
        # The extended-length prefix also removes reserved-name files
        # (a stray literal "nul" from mis-redirected compile output).
        Remove-Item -LiteralPath ("\\?\" + $artifacts) -Recurse -Force
        Write-Host 'Cleaned: artifacts\ removed.'
    } else {
        Write-Host 'Clean: nothing to remove.'
    }
    if (-not $BuildDictionary) {
        $BuildDictionary = $true
        Write-Host 'Clean build: the dictionary will be rebuilt.'
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $vsPath) {
    throw 'MSVC x86/x64 build tools were not found.'
}

$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
$targets = if ($Platform -eq 'Both') { @('Win32', 'x64') } else { @($Platform) }

foreach ($target in $targets) {
    & $msbuild (Join-Path $root 'Lekhoni.sln') `
        /m /t:Build "/p:Configuration=$Configuration" "/p:Platform=$target" /v:minimal

    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for $target."
    }

    $imeDll = Join-Path $root "artifacts\$target\$Configuration\LekhoniIME.dll"
    if ((Get-Item -LiteralPath $imeDll).Length -gt 1MB) {
        throw "IME DLL exceeds the 1 MB release budget for $target."
    }
}

# --- dictionaries and the layout table -------------------------------------
# They are produced before the test loop because the end-to-end and audit
# suites validate the exact images this build ships.

$compilerPlatform = if ($targets -contains 'x64') { 'x64' } else { 'Win32' }
$compiler = Join-Path $root "artifacts\$compilerPlatform\$Configuration\LekhoniDictionaryCompiler.exe"
$coreOutput = Join-Path $root 'artifacts\LekhoniCore.bgk'

$coreSource = $null
$coreSourceIsCompiled = $false
if ($UseStarterDictionary) {
    $coreSource = Join-Path $root 'data\source\starter.tsv'
} elseif ($DictionarySource) {
    $coreSource = if ([IO.Path]::IsPathRooted($DictionarySource)) {
        $DictionarySource
    } else {
        Join-Path $root $DictionarySource
    }
} elseif (Test-Path -LiteralPath (Join-Path $root 'data\source\imported-core.tsv') -PathType Leaf) {
    # The key-augmented corpus for the Avro layout engine: the merged corpus
    # with one generated phonetic key per word (GPL-derived data, see data\README.md).
    $coreSource = Join-Path $root 'data\source\imported-core.tsv'
} elseif (Test-Path -LiteralPath (Join-Path $root 'data\compiled\LekhoniCore.merged.bgk') -PathType Leaf) {
    $coreSource = Join-Path $root 'data\compiled\LekhoniCore.merged.bgk'
    $coreSourceIsCompiled = $true
}

if ($coreSource) {
    if ($coreSourceIsCompiled) {
        $bytes = [IO.File]::ReadAllBytes($coreSource)
        if ($bytes.Length -lt 96 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'BGDX') {
            throw "$coreSource is not a valid BGDX dictionary."
        }
        Write-Host "Using precompiled merged dictionary: $coreSource"
        Copy-Item -LiteralPath $coreSource -Destination $coreOutput -Force
    } else {
        $coreSource = (Resolve-Path -LiteralPath $coreSource -ErrorAction Stop).Path
        Write-Host "Compiling core dictionary: $coreSource"
        & $compiler $coreSource $coreOutput
        if ($LASTEXITCODE -ne 0) {
            throw 'Dictionary compilation failed.'
        }
    }
} elseif ($BuildDictionary) {
    throw 'No dictionary source found (data\source\imported-core.tsv, data\compiled\LekhoniCore.merged.bgk, or data\source\starter.tsv).'
}

if ((Test-Path -LiteralPath $coreOutput -PathType Leaf)) {
    $hashes = @()
    foreach ($target in $targets) {
        $destination = Join-Path $root "artifacts\$target\$Configuration\LekhoniCore.bgk"
        Copy-Item -LiteralPath $coreOutput -Destination $destination -Force
        $hashes += (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    }
    if (($hashes | Select-Object -Unique).Count -ne 1) {
        throw 'Dictionary output differs between target architectures.'
    }
}

# The phrase layer is compiled independently of the core dictionary so that
# editing a phrase never requires regenerating a large corpus. It is optional:
# when data\source\phrases.tsv is absent the IME simply runs without it.
$phraseSource = Join-Path $root 'data\source\phrases.tsv'
if (Test-Path -LiteralPath $phraseSource -PathType Leaf) {
    $phraseOutput = Join-Path $root 'artifacts\LekhoniPhrases.bgk'
    # The Avro autocorrect conversions (data\source\imported-autodict.tsv,
    # MPL-2.0/GPL-derived) are
    # compiled into the phrase layer: they behave exactly like curated phrases
    # (exact-key overrides) and stay out of the fuzzy search. A starter build
    # skips the merge so the package stays MIT-only.
    $phraseInput = $phraseSource
    $autodictSource = Join-Path $root 'data\source\imported-autodict.tsv'
    if (-not $UseStarterDictionary -and (Test-Path -LiteralPath $autodictSource -PathType Leaf)) {
        $phraseInput = Join-Path $root 'artifacts\phrase-combined.tsv'
        Get-Content -LiteralPath $phraseSource | Set-Content -LiteralPath $phraseInput
        Get-Content -LiteralPath $autodictSource | Add-Content -LiteralPath $phraseInput
        $capsSource = Join-Path $root 'data\source\imported-autodict-caps.tsv'
        if (Test-Path -LiteralPath $capsSource -PathType Leaf) {
            Get-Content -LiteralPath $capsSource | Add-Content -LiteralPath $phraseInput
        }
        Write-Host 'Phrase layer includes the Avro autocorrect conversions.'
    }
    Write-Host "Compiling curated phrase layer: $phraseInput"
    # Compile $phraseInput, not $phraseSource. They differ whenever the
    # autocorrect file above was merged in, and compiling the source alone
    # silently shipped a 38-word phrase layer while reporting the combined file
    # on screen: the 5011 autocorrect conversions were written to disk and then
    # discarded, and the build still printed that they were included.
    & $compiler $phraseInput $phraseOutput
    if ($LASTEXITCODE -ne 0) {
        throw 'Phrase dictionary compilation failed.'
    }
    foreach ($target in $targets) {
        $destination = Join-Path $root "artifacts\$target\$Configuration\LekhoniPhrases.bgk"
        Copy-Item -LiteralPath $phraseOutput -Destination $destination -Force
    }
} else {
    Write-Host 'No data\source\phrases.tsv present; the phrase layer will not be built.'
    foreach ($target in $targets) {
        $stale = Join-Path $root "artifacts\$target\$Configuration\LekhoniPhrases.bgk"
        Remove-Item -LiteralPath $stale -Force -ErrorAction SilentlyContinue
    }
}

# The Avro Phonetic layout table (BGLT1, compiled from third-party GPL data by
# tools\compile_avro_layout.py) is runtime data: without it the IME falls back
# to the built-in rules. The repository copy is the source of truth. A starter
# build skips it (and removes stale copies) so the package stays MIT-only.
$layoutSource = Join-Path $root 'data\compiled\avro-layout.dat'
if ($UseStarterDictionary) {
    Write-Host 'Starter build: the derived Avro layout table is skipped; the IME will use its built-in rules.'
    Remove-Item -LiteralPath (Join-Path $root 'artifacts\avro-layout.dat') -Force -ErrorAction SilentlyContinue
    foreach ($target in $targets) {
        Remove-Item -LiteralPath (Join-Path $root "artifacts\$target\$Configuration\AvroLayout.dat") -Force -ErrorAction SilentlyContinue
    }
} elseif (Test-Path -LiteralPath $layoutSource -PathType Leaf) {
    Copy-Item -LiteralPath $layoutSource -Destination (Join-Path $root 'artifacts\avro-layout.dat') -Force
    foreach ($target in $targets) {
        Copy-Item -LiteralPath $layoutSource `
            -Destination (Join-Path $root "artifacts\$target\$Configuration\AvroLayout.dat") -Force
    }
} else {
    Write-Host 'Warning: data\compiled\avro-layout.dat is missing; the IME will use the built-in rules.'
}

# The trigram next-word table (BGTG1) is optional runtime data; without it
# the IME falls back to the static hint lists. A starter build skips it too
# (it is derived from CC BY-SA Wikipedia text).
$trigramSource = Join-Path $root 'data\compiled\LekhoniTrigrams.dat'
if ($UseStarterDictionary) {
    Write-Host 'Starter build: the derived trigram table is skipped; next-word prediction uses the static hint lists.'
    Remove-Item -LiteralPath (Join-Path $root 'artifacts\LekhoniTrigrams.dat') -Force -ErrorAction SilentlyContinue
    foreach ($target in $targets) {
        Remove-Item -LiteralPath (Join-Path $root "artifacts\$target\$Configuration\LekhoniTrigrams.dat") -Force -ErrorAction SilentlyContinue
    }
} elseif (Test-Path -LiteralPath $trigramSource -PathType Leaf) {
    Copy-Item -LiteralPath $trigramSource -Destination (Join-Path $root 'artifacts\LekhoniTrigrams.dat') -Force
    foreach ($target in $targets) {
        Copy-Item -LiteralPath $trigramSource `
            -Destination (Join-Path $root "artifacts\$target\$Configuration\LekhoniTrigrams.dat") -Force
    }
} else {
    Write-Host 'Warning: data\compiled\LekhoniTrigrams.dat is missing; context-aware next-word prediction is disabled.'
}

# --- tests ------------------------------------------------------------------

if ($RunTests) {
    $coreImage = Join-Path $root 'artifacts\LekhoniCore.bgk'
    $phraseImage = Join-Path $root 'artifacts\LekhoniPhrases.bgk'
    $haveCoreImage = Test-Path -LiteralPath $coreImage -PathType Leaf

    foreach ($target in $targets) {
        $test = Join-Path $root "artifacts\$target\$Configuration\LekhoniCoreTests.exe"
        & $test
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed for $target."
        }

        $imeSmokeTest = Join-Path $root "artifacts\$target\$Configuration\LekhoniIMESmokeTests.exe"
        & $imeSmokeTest
        if ($LASTEXITCODE -ne 0) {
            throw "IME smoke tests failed for $target."
        }

        # The robustness suite reads tests\conformance.txt, so it runs from the
        # tests directory. The end-to-end suite is given explicit paths.
        $robustnessTest = Join-Path $root "artifacts\$target\$Configuration\LekhoniRobustnessTests.exe"
        $endToEndTest = Join-Path $root "artifacts\$target\$Configuration\LekhoniEndToEndTests.exe"
        Push-Location (Join-Path $root 'tests')
        try {
            & $robustnessTest
            if ($LASTEXITCODE -ne 0) {
                throw "Robustness tests failed for $target."
            }

            if (-not $haveCoreImage) {
                Write-Host 'Skipping image-dependent suites: no core dictionary was built.'
                continue
            }

            $phraseArgument = if (Test-Path -LiteralPath $phraseImage -PathType Leaf) {
                $phraseImage
            } else {
                (Join-Path $root 'nonexistent.bgk')
            }
            & $endToEndTest $coreImage $phraseArgument
            if ($LASTEXITCODE -ne 0) {
                throw "End-to-end tests failed for $target."
            }

            # The audit runs against the images this build actually produced, so
            # a dictionary that compiles but is unusable is caught here rather
            # than by a user. The second core image is the other architecture's
            # copy, when one is on disk, to check byte parity.
            $auditTest = Join-Path $root "artifacts\$target\$Configuration\LekhoniDictionaryAuditTests.exe"
            $alternateCore = if ($targets.Count -gt 1) {
                $other = if ($target -eq 'x64') { 'Win32' } else { 'x64' }
                Join-Path $root "artifacts\$other\$Configuration\LekhoniCore.bgk"
            } else {
                ''
            }
            $alternateArgument = if ($alternateCore -and (Test-Path -LiteralPath $alternateCore -PathType Leaf)) {
                $alternateCore
            } else {
                ''
            }
            & $auditTest $coreImage $phraseArgument $alternateArgument
            if ($LASTEXITCODE -ne 0) {
                throw "Dictionary audit failed for $target."
            }

            # The audit above checks that the images are well formed. This
            # measures whether they are *accurate*: it types the hand-checked
            # pairs in data\source\eval.tsv and reports top-1, top-5 and MRR, so a
            # change to the rules, the ranking, or the corpus is visible as a
            # number instead of a feeling. It reports rather than gates: the
            # set is a curated seed, not a benchmark, so its absolute values
            # mean little on their own. Only the delta between two builds does.
            $evalSet = Join-Path $root 'data\source\eval.tsv'
            if (Test-Path -LiteralPath $evalSet -PathType Leaf) {
                # Pass the layout table so the numbers describe the
                # transliteration path the IME actually uses, not the fallback.
                # A starter build omits it on purpose: that package ships
                # without the table, so its numbers must describe the
                # built-in-rules path.
                $evalLayout = Join-Path $root 'data\compiled\avro-layout.dat'
                $layoutArgument = if (-not $UseStarterDictionary -and (Test-Path -LiteralPath $evalLayout -PathType Leaf)) {
                    $evalLayout
                } else {
                    (Join-Path $root 'nonexistent.dat')
                }
                Write-Host ''
                Write-Host 'Accuracy against data\source\eval.tsv:'
                & $compiler --score $evalSet $coreImage $phraseArgument $layoutArgument
                if ($LASTEXITCODE -ne 0) {
                    throw "Accuracy evaluation failed for $target."
                }
            }
        } finally {
            Pop-Location
        }
    }
}
