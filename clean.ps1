[CmdletBinding()]
param()

# Removes every generated build output: intermediate objects, test binaries,
# dictionaries, the layout-table copies, and packages. Sources under src\,
# tests\, tools\, and data\ are untouched. Run build.ps1 afterwards to rebuild.
# The compiled dictionary and layout table are regenerated from the checked-in
# data (data\source\imported-core.tsv, data\compiled\avro-layout.dat) by build.ps1.

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$artifacts = Join-Path $root 'artifacts'

if (Test-Path -LiteralPath $artifacts) {
    # The extended-length prefix also removes reserved-name files (a
    # stray literal "nul" from mis-redirected compile output).
    Remove-Item -LiteralPath ("\\?\" + $artifacts) -Recurse -Force
    Write-Host "Cleaned: $artifacts removed."
} else {
    Write-Host 'Already clean: no artifacts directory.'
}

Write-Host 'Run .\build.ps1 -RunTests to rebuild everything.'
